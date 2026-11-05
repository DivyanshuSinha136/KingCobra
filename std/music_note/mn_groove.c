/* KingCobra std.music_note — genre generators: phonk (Memphis / drift / Brazilian) and funk.
 *
 * groove(style, options)  -> notation text (readable, editable, reproducible from its seed)
 * produce(style, options) -> mastered stereo audio (stems rendered, kick sidechain, master preset)
 * stems(style, options)   -> the individual tracks as audio, ready for a DAW
 *
 * The generators write ordinary std.music_note notation, one track per role (kick, snare, hats, perc,
 * bass, ...), so you can parse() the text, edit it, re-render it or export it with to_midi().
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mn_internal.h"
#include <stdarg.h>

#define SETK(o, k, v) kc_object_set((o), (k), strlen(k), (v))

/* ============================================================ small utilities */

typedef struct { char *s; size_t n, cap; } SB;
static void sb_add(SB *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[512]; int need = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (need < 0) return;
    if (b->n + (size_t)need + 1 > b->cap) { b->cap = (b->n + (size_t)need + 1) * 2 + 256; b->s = kc_realloc(b->s, b->cap); }
    if ((size_t)need < sizeof tmp) memcpy(b->s + b->n, tmp, (size_t)need + 1);
    else { va_start(ap, fmt); vsnprintf(b->s + b->n, (size_t)need + 1, fmt, ap); va_end(ap); }
    b->n += (size_t)need;
}
static void sb_free(SB *b) { free(b->s); b->s = NULL; b->n = b->cap = 0; }

static double ur(MnRng *r) { return (mn_rng_f(r) + 1.0) * 0.5; }
static int ri(MnRng *r, int n) { int v = (int)(ur(r) * n); return v >= n ? n - 1 : v; }
static bool chance(MnRng *r, double p) { return ur(r) < p; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- drum bar: 16 sixteenth steps, each may hold 1..4 sub-hits with a velocity ramp (rolls) ---- */
typedef struct { int vel[16], sub[16], ramp[16]; } DBar;
static void dbar_clear(DBar *d) { for (int i = 0; i < 16; i++) { d->vel[i] = 0; d->sub[i] = 1; d->ramp[i] = 0; } }
static void emit_rest(SB *b, int steps) { if (steps > 0) sb_add(b, "r:%gb ", steps * 0.25); }
static void emit_dbar(SB *b, const char *drum, const DBar *d) {
    int rest = 0;
    for (int i = 0; i < 16; i++) {
        if (d->vel[i] <= 0) { rest++; continue; }
        emit_rest(b, rest); rest = 0;
        int k = d->sub[i] < 1 ? 1 : d->sub[i];
        for (int j = 0; j < k; j++) {
            int v = clampi(d->vel[i] + (k > 1 ? d->ramp[i] * j / (k - 1) : 0), 1, 127);
            if (k == 1) sb_add(b, "%s:s@%d ", drum, v);
            else sb_add(b, "%s:%.9fb@%d ", drum, 0.25 / k, v);
        }
    }
    emit_rest(b, rest);
    sb_add(b, "|\n");
}

/* ---- melodic bar: monophonic list of notes / chords placed on the 16-step grid ---- */
typedef struct { int step, len, vel, nm, midi[6]; bool stac, slide; } NEv;
typedef struct { NEv e[48]; int n; } MBar;
static void mbar_note(MBar *m, int step, int len, int midi, int vel, bool stac) {
    if (m->n >= 48) return;
    NEv *e = &m->e[m->n++]; e->step = step; e->len = len; e->vel = vel; e->nm = 1; e->midi[0] = midi; e->stac = stac; e->slide = false;
}
static void mbar_chord(MBar *m, int step, int len, const int *midi, int nm, int vel, bool stac) {
    if (m->n >= 48) return;
    NEv *e = &m->e[m->n++]; e->step = step; e->len = len; e->vel = vel; e->nm = nm > 6 ? 6 : nm; e->stac = stac; e->slide = false;
    for (int i = 0; i < e->nm; i++) e->midi[i] = midi[i];
}
static void emit_mbar(SB *b, const MBar *m) {
    int cur = 0; char nb[16];
    for (int i = 0; i < m->n; i++) {
        const NEv *e = &m->e[i];
        if (e->step < cur) continue;
        emit_rest(b, e->step - cur);
        int len = e->len; int limit = (i + 1 < m->n && m->e[i + 1].step > e->step) ? m->e[i + 1].step : 16;
        if (e->step + len > limit) len = limit - e->step;
        if (len < 1) len = 1;
        char suf[16]; snprintf(suf, sizeof suf, "%s", e->slide ? "^" : e->stac ? "'" : "");
        if (e->nm == 1) sb_add(b, "%s:%gb@%d%s ", mn_midi_name(e->midi[0], false, nb), len * 0.25, e->vel, suf);
        else {
            sb_add(b, "[");
            for (int k = 0; k < e->nm; k++) sb_add(b, "%s%s", k ? " " : "", mn_midi_name(e->midi[k], false, nb));
            sb_add(b, "]:%gb@%d%s ", len * 0.25, e->vel, suf);
        }
        cur = e->step + len;
    }
    emit_rest(b, 16 - cur);
    sb_add(b, "|\n");
}
static int midi_in(int pc, int lo) { int m = lo + (((pc - lo) % 12) + 12) % 12; return m; }   /* the pitch class pc at or above lo */

/* ============================================================ options */

typedef enum { G_PHONK, G_DRIFT, G_BRAZIL, G_FUNK } Kind;
typedef struct { Kind kind; int bars; double bpm; int root_pc; bool minor; char key[24]; double intensity, swing16; uint64_t seed; bool phrygian; } GOpt;

static const char *STYLES[] = { "phonk", "drift_phonk", "brazilian_phonk", "funk" };

static bool style_from(const char *s, Kind *k) {
    if (!strcasecmp(s, "phonk") || !strcasecmp(s, "memphis") || !strcasecmp(s, "memphis_phonk")) { *k = G_PHONK; return true; }
    if (!strcasecmp(s, "drift_phonk") || !strcasecmp(s, "drift")) { *k = G_DRIFT; return true; }
    if (!strcasecmp(s, "brazilian_phonk") || !strcasecmp(s, "brazilian") || !strcasecmp(s, "phonk_br") || !strcasecmp(s, "funk_carioca")) { *k = G_BRAZIL; return true; }
    if (!strcasecmp(s, "funk")) { *k = G_FUNK; return true; }
    return false;
}

static void parse_gopt(KcValue style, KcValue o, GOpt *g, KcPos pos, const char *who) {
    const char *sn = mn_str(style, pos, who, "style");
    if (!style_from(sn, &g->kind)) kc_error_at(pos, "music_note.%s: unknown style '%s' (see music_note.groove_styles(): phonk, drift_phonk, brazilian_phonk, funk)", who, sn);
    static const double BPM[] = { 138, 150, 130, 104 };
    static const char *KEY[] = { "F#m", "Dm", "Gm", "Em" };
    g->bars = (int)mn_opt_int(o, "bars", 16, pos, who);
    g->bpm = mn_opt_num(o, "bpm", BPM[g->kind], pos, who);
    g->intensity = mn_opt_num(o, "intensity", 0.75, pos, who);
    g->seed = (uint64_t)mn_opt_int(o, "seed", 1, pos, who);
    g->swing16 = mn_opt_num(o, "swing", g->kind == G_FUNK ? 0.12 : 0.0, pos, who);
    g->phrygian = !strcasecmp(mn_opt_str(o, "scale", "minor", pos, who), "phrygian");
    snprintf(g->key, sizeof g->key, "%s", mn_opt_str(o, "key", KEY[g->kind], pos, who));
    if (g->bars < 1 || g->bars > 128) kc_error_at(pos, "music_note.%s: bars must be 1..128", who);
    if (g->bpm < 60 || g->bpm > 220) kc_error_at(pos, "music_note.%s: bpm must be 60..220", who);
    if (g->intensity < 0 || g->intensity > 1) kc_error_at(pos, "music_note.%s: intensity must be 0..1", who);
    int f; bool mi;
    if (!mn_parse_key(g->key, &f, &mi)) kc_error_at(pos, "music_note.%s: unknown key '%s' (examples: F#m, Dm, Gm, Em, C)", who, g->key);
    MnNote nn; const char *e = mn_note_scan(g->key, &nn);
    if (!e) kc_error_at(pos, "music_note.%s: unknown key '%s'", who, g->key);
    g->root_pc = (((nn.pc + nn.acc) % 12) + 12) % 12; g->minor = mi;
}

typedef enum { BT_INTRO, BT_MAIN, BT_FILL, BT_BREAK } BarType;
static BarType bar_type(int b, int B) {
    if (B >= 8 && b < 2) return BT_INTRO;
    if (B >= 16 && (b % 16 == 12 || b % 16 == 13)) return BT_BREAK;
    if (b == B - 1 || (B >= 8 && b % 8 == 7)) return BT_FILL;
    return BT_MAIN;
}

/* ============================================================ phonk */

static const int PH_PROG[6][4] = { { 0, 8, 10, 7 }, { 0, 10, 8, 10 }, { 0, 5, 8, 10 }, { 0, 0, 8, 10 }, { 0, 3, 8, 10 }, { 0, 7, 8, 10 } };
static const int KICK_PHONK[5][6] = { { 0, 7, 10, -1 }, { 0, 6, 7, 11, -1 }, { 0, 3, 8, 10, -1 }, { 0, 7, 10, 14, -1 }, { 0, 6, 10, 12, -1 } };
static const int KICK_DRIFT[3][7] = { { 0, 3, 6, 10, 12, -1 }, { 0, 2, 6, 7, 10, 14, -1 }, { 0, 3, 6, 8, 11, 14, -1 } };
static const int KICK_BRAZIL[2][8] = { { 0, 3, 6, 9, 12, -1 }, { 0, 3, 6, 10, 12, 14, -1 } };
static const int PENT[7] = { 0, 3, 5, 7, 10, 12, 15 };

static bool chord_is_minor(int off) { return off == 0 || off == 5 || off == 7; }

static void gen_phonk(SB *tr, MnRng *R, const GOpt *g) {
    /* tr[0..7] = kick snare hats perc bass lead bells pad */
    int B = g->bars, prog = ri(R, 6);
    int kt = ri(R, g->kind == G_DRIFT ? 3 : g->kind == G_BRAZIL ? 2 : 5);
    int cell_t = ri(R, 5);
    static const int CELLS[5][5] = { { 0, 3, 6, -1, -1 }, { 0, 2, 4, 7, -1 }, { 0, 3, 4, 7, -1 }, { 0, 1, 4, 6, -1 }, { 0, 4, 6, -1, -1 } };
    int cn = 0; while (CELLS[cell_t][cn] >= 0) cn++;
    int cell_deg[5];
    for (int i = 0; i < cn; i++) cell_deg[i] = (i == 0) ? 0 : ri(R, g->phrygian ? 6 : 5) ;
    int cell_alt = ri(R, cn);
    double I = g->intensity;
    int kick_list[16], nk = 0;
    for (int b = 0; b < B; b++) {
        BarType bt = bar_type(b, B);
        int off = PH_PROG[prog][b % 4], rootm = midi_in(g->root_pc + off, 28);
        bool cmin = chord_is_minor(off);
        /* ---- kick pattern ---- */
        nk = 0;
        const int *kp = g->kind == G_DRIFT ? KICK_DRIFT[kt % 3] : g->kind == G_BRAZIL ? KICK_BRAZIL[kt % 2] : KICK_PHONK[kt];
        for (int i = 0; kp[i] >= 0; i++) kick_list[nk++] = kp[i];
        if (bt == BT_MAIN && chance(R, 0.3 * I)) { static const int X[4] = { 3, 12, 14, 15 }; int s = X[ri(R, 4)]; bool has = false; for (int i = 0; i < nk; i++) if (kick_list[i] == s) has = true; if (!has && nk < 15) kick_list[nk++] = s; }
        for (int i = 0; i < nk; i++) for (int j = i + 1; j < nk; j++) if (kick_list[j] < kick_list[i]) { int t = kick_list[i]; kick_list[i] = kick_list[j]; kick_list[j] = t; }

        DBar kk, sn, hh, pc, sp; dbar_clear(&kk); dbar_clear(&sn); dbar_clear(&hh); dbar_clear(&pc); dbar_clear(&sp);
        bool drums_on = (bt != BT_INTRO && bt != BT_BREAK) || (bt == BT_INTRO && b == 1);
        if (bt == BT_MAIN || bt == BT_FILL) for (int i = 0; i < nk; i++) kk.vel[kick_list[i]] = kick_list[i] == 0 ? 120 : 100 + ri(R, 12);
        if (bt == BT_FILL) { for (int i = 8; i < 16; i++) { sn.vel[i] = 70 + (i - 8) * 7; } sn.vel[8] = 118; }
        else if (bt == BT_MAIN) {
            sn.vel[8] = 118; sp.vel[8] = 100;
            if (g->kind == G_DRIFT && chance(R, 0.5)) sn.vel[15] = 55;
            if (g->kind == G_BRAZIL) { sn.vel[8] = 0; sn.vel[4] = 105; sn.vel[12] = 105; sp.vel[4] = 90; sp.vel[12] = 90; }
        }
        /* ---- hats: 16ths with accents, gaps and rolls ---- */
        if (bt != BT_INTRO || b == 1) {
            for (int i = 0; i < 16; i++) {
                int v = i % 4 == 0 ? 92 : i % 2 == 0 ? 74 : 52;
                if (bt == BT_BREAK) v = 38 + (i % 2 ? 0 : 12);
                if (g->kind == G_BRAZIL && i % 2 == 1) v = 0;
                if (v && bt == BT_MAIN && chance(R, 0.10 * (1.0 - I) + 0.04)) v = 0;
                hh.vel[i] = v;
            }
            if (bt == BT_MAIN && chance(R, (g->kind == G_DRIFT ? 0.8 : 0.5) * I)) {
                static const int RS[6] = { 3, 6, 7, 11, 14, 15 }; int s = RS[ri(R, 6)];
                hh.vel[s] = 60; hh.sub[s] = chance(R, 0.5) ? 3 : 4; hh.ramp[s] = 40;
            }
            if (bt == BT_FILL) { for (int i = 12; i < 16; i++) { hh.vel[i] = 55 + (i - 12) * 10; hh.sub[i] = 3; hh.ramp[i] = 35; } }
        }
        /* ---- perc: cowbell offbeats + open hat ---- */
        if (bt == BT_MAIN) {
            static const int CB[4] = { 2, 6, 10, 14 };
            for (int i = 0; i < 4; i++) if (chance(R, 0.55 + 0.2 * I)) pc.vel[CB[i]] = 55 + ri(R, 25);
            if (chance(R, 0.4)) sp.vel[10] = 0;
        }
        emit_dbar(&tr[0], "kick", drums_on ? &kk : &(DBar){ 0 });
        /* the DBar{0} compound literal has sub=0, treated as 1; vel=0 -> all rests */
        emit_dbar(&tr[1], "snare", drums_on ? &sn : &(DBar){ 0 });
        emit_dbar(&tr[2], "hat", &hh);
        { /* perc track = cowbell hits; snap layered on the snare lives with perc too */
          DBar pp; dbar_clear(&pp);
          for (int i = 0; i < 16; i++) { pp.vel[i] = pc.vel[i]; }
          emit_dbar(&tr[3], "cowbell", drums_on ? &pp : &(DBar){ 0 });
        }
        { DBar snp; dbar_clear(&snp); if (drums_on) for (int i = 0; i < 16; i++) snp.vel[i] = sp.vel[i]; emit_dbar(&tr[8], "snap", &snp); }

        /* ---- 808 ---- */
        {
            MBar m; m.n = 0;
            if (bt == BT_BREAK) mbar_note(&m, 0, 16, rootm, 105, false);
            else if (bt == BT_INTRO) { if (b == 1) mbar_note(&m, 0, 16, rootm, 100, false); }
            else {
                for (int i = 0; i < nk; i++) {
                    int st = kick_list[i], len = (i + 1 < nk ? kick_list[i + 1] : 16) - st, note = rootm;
                    if (i > 0 && chance(R, 0.18 * I + 0.05)) { static const int AL[3] = { 7, 12, 3 }; note = rootm + AL[ri(R, 3)]; if (note > 43) note -= 12; }
                    if (len > 8) len = 8;
                    mbar_note(&m, st, len, note, 108, false);
                }
                if (m.n > 0 && chance(R, 0.4)) m.e[m.n - 1].slide = true;          /* slide into the next bar's note */
                if (m.n > 1 && chance(R, 0.25 * I)) m.e[m.n - 2].slide = true;
            }
            emit_mbar(&tr[4], &m);
        }
        /* ---- cowbell lead ---- */
        {
            MBar m; m.n = 0;
            int base = midi_in(g->root_pc, 72);
            int tr_off = (off == 0) ? 0 : (off > 6 ? off - 12 : off);
            if (bt == BT_MAIN || bt == BT_FILL || (bt == BT_INTRO && b == 1)) {
                for (int half = 0; half < 2; half++) {
                    if (bt == BT_INTRO && half == 0) continue;
                    for (int i = 0; i < cn; i++) {
                        int deg = cell_deg[i]; if (half == 1 && i == cell_alt) deg = (deg + 2) % 5;
                        int pitch = base + PENT[deg] + (bt == BT_FILL && half == 1 ? 0 : tr_off) + (g->phrygian && deg == 1 ? -2 : 0);
                        if (!cmin && deg == 1) pitch += 0;
                        int st = half * 8 + CELLS[cell_t][i], nxt = (i + 1 < cn) ? half * 8 + CELLS[cell_t][i + 1] : half * 8 + 8;
                        mbar_note(&m, st, nxt - st, pitch, 78 + ri(R, 12), false);
                    }
                }
            }
            emit_mbar(&tr[5], &m);
        }
        /* ---- bells: sparse eerie arpeggio ---- */
        {
            MBar m; m.n = 0;
            int r5 = midi_in(g->root_pc + off, 72), third = r5 + (cmin ? 3 : 4), fifth = r5 + 7;
            const int arp[4] = { r5, third, fifth, r5 + 12 };
            if (bt != BT_BREAK && bt != BT_FILL && (b % 2 == 0 || I > 0.7)) {
                static const int ST[5] = { 0, 3, 6, 9, 12 };
                for (int i = 0; i < 5; i++) if (i == 0 || chance(R, 0.55)) mbar_note(&m, ST[i], 3, arp[ri(R, 4)], 50 + ri(R, 15), false);
            }
            emit_mbar(&tr[6], &m);
        }
        /* ---- pad ---- */
        {
            MBar m; m.n = 0;
            int r3 = midi_in(g->root_pc + off, 48); int ch[3] = { r3, r3 + (cmin ? 3 : 4), r3 + 7 };
            mbar_chord(&m, 0, 16, ch, 3, 62, false);
            emit_mbar(&tr[7], &m);
        }
    }
}

/* ============================================================ funk */

typedef struct { int root; int quality; /* 0 m7, 1 dom9, 2 dom7 */ } FChord;
static void funk_chord_notes(const FChord *c, int lo, int *out, int *n) {
    int r = midi_in(c->root, lo);
    static const int M7[4] = { 0, 3, 7, 10 }, D9[4] = { 0, 4, 10, 14 }, D7[4] = { 0, 4, 7, 10 };
    const int *iv = c->quality == 0 ? M7 : c->quality == 1 ? D9 : D7;
    for (int i = 0; i < 4; i++) out[i] = r + iv[i];
    *n = 4;
}

static void gen_funk(SB *tr, MnRng *R, const GOpt *g) {
    /* tr[0..8] = kick snare hats perc bass keys guitar horns lead */
    int B = g->bars; double I = g->intensity;
    FChord vamp[4]; int nv;
    int vt = ri(R, 4); int r = g->root_pc;
    int q_tonic = g->minor ? 0 : 1;
    switch (vt) {
        case 0: vamp[0] = (FChord){ r, q_tonic }; vamp[1] = (FChord){ (r + 5) % 12, 1 }; nv = 2; break;          /* i7 -> IV9 */
        case 1: vamp[0] = (FChord){ r, q_tonic }; nv = 1; break;                                                  /* one-chord vamp */
        case 2: vamp[0] = (FChord){ r, q_tonic }; vamp[1] = (FChord){ (r + 10) % 12, 1 }; vamp[2] = (FChord){ (r + 5) % 12, 1 }; vamp[3] = (FChord){ (r + 10) % 12, 1 }; nv = 4; break;
        default: vamp[0] = (FChord){ r, q_tonic }; vamp[1] = (FChord){ r, q_tonic }; vamp[2] = (FChord){ (r + 5) % 12, 1 }; vamp[3] = (FChord){ (r + 7) % 12, 2 }; nv = 4; break;
    }
    /* kick: the one + syncopations */
    static const double KW[16] = { 0, 0, 0.15, 0.35, 0, 0, 0.3, 0.3, 0, 0, 0.45, 0.35, 0, 0, 0.25, 0.1 };
    int kicks[16], nk = 0; kicks[nk++] = 0;
    int want = 2 + ri(R, 2);
    for (int tries = 0; tries < 40 && nk < want + 1; tries++) { int s = ri(R, 16); if (KW[s] > 0 && chance(R, KW[s] * 1.6)) { bool has = false; for (int i = 0; i < nk; i++) if (kicks[i] == s) has = true; if (!has) kicks[nk++] = s; } }
    /* bass rhythm mask (sixteenth steps that sound) and pitch choices */
    static const double BW[16] = { 1, 0.15, 0.4, 0.55, 0.2, 0.2, 0.5, 0.45, 0.3, 0.15, 0.55, 0.5, 0.2, 0.15, 0.45, 0.4 };
    int bmask[16]; for (int i = 0; i < 16; i++) bmask[i] = (i == 0) || chance(R, BW[i] * (0.6 + 0.7 * I));
    for (int i = 0; i < nk; i++) bmask[kicks[i]] = 1;
    /* comp stabs */
    static const double CW[16] = { 0.1, 0.1, 0.4, 0.35, 0.1, 0.25, 0.4, 0.35, 0.1, 0.3, 0.5, 0.3, 0.1, 0.3, 0.4, 0.2 };
    int cmask[16]; int nc = 0; for (int i = 0; i < 16; i++) { cmask[i] = chance(R, CW[i] * (0.8 + 0.6 * I)); nc += cmask[i]; }
    if (nc < 3) { cmask[3] = cmask[6] = cmask[10] = 1; }
    int horn_a = 10, horn_b = 14, horn_c = 3;
    for (int b = 0; b < B; b++) {
        BarType bt = bar_type(b, B); if (bt == BT_BREAK) bt = BT_MAIN;
        const FChord *ch = &vamp[(b % 4) % nv]; if (nv == 2) ch = &vamp[b % 2];
        int rootm = midi_in(ch->root, 28), cn[4], ncn; funk_chord_notes(ch, 62, cn, &ncn);
        bool intro = bt == BT_INTRO;
        /* ---- drums ---- */
        DBar kk, sn, hh, pc, ck; dbar_clear(&kk); dbar_clear(&sn); dbar_clear(&hh); dbar_clear(&pc); dbar_clear(&ck);
        for (int i = 0; i < nk; i++) kk.vel[kicks[i]] = kicks[i] == 0 ? 120 : 98 + ri(R, 14);
        sn.vel[4] = 112; sn.vel[12] = 116;
        static const int GH[6] = { 3, 7, 9, 11, 14, 15 };
        for (int i = 0; i < 6; i++) if (chance(R, 0.38 * (0.4 + I))) sn.vel[GH[i]] = 26 + ri(R, 18);
        if (bt == BT_FILL) { for (int i = 12; i < 16; i++) { sn.vel[i] = 70 + (i - 12) * 14; } sn.vel[12] = 116; }
        for (int i = 0; i < 16; i++) hh.vel[i] = i % 4 == 0 ? 100 : i % 4 == 2 ? 78 : (i % 2 ? 46 : 60);
        if (chance(R, 0.5 + 0.2 * I) && bt != BT_FILL) { hh.vel[14] = 0; hh.vel[15] = 0; ck.vel[14] = 92; }      /* open hat on the 'and' of 4 */
        if (chance(R, 0.2)) hh.vel[ri(R, 8) * 2 + 1] = 0;
        for (int i = 0; i < 16; i += 2) pc.vel[i + 1 > 15 ? i : i + 1] = 34 + ri(R, 14);                          /* shaker on the e-and-a */
        emit_dbar(&tr[0], "kick", &kk);
        emit_dbar(&tr[1], "snare", &sn);
        emit_dbar(&tr[2], "hat", &hh);
        { DBar o; dbar_clear(&o); for (int i = 0; i < 16; i++) o.vel[i] = ck.vel[i]; emit_dbar(&tr[9], "ohat", &o); }
        emit_dbar(&tr[3], "shaker", &pc);

        /* ---- bass ---- */
        {
            MBar m; m.n = 0;
            for (int i = 0; i < 16; i++) {
                if (!bmask[i]) continue;
                int note = rootm, vel = 100, len = 1; bool stac = true;
                if (i == 0) { vel = 118; len = 2 + (chance(R, 0.5) ? 1 : 0); stac = false; }
                else {
                    int ch2 = ri(R, 10);
                    note = ch2 < 3 ? rootm + 12 : ch2 < 5 ? rootm + 7 : ch2 < 7 ? rootm + 10 : ch2 < 8 ? rootm + 5 : ch2 < 9 ? rootm + (ch->quality == 0 ? 3 : 4) : rootm;
                    if (chance(R, 0.18)) { vel = 38; }                 /* ghost / dead note */
                    else vel = 84 + ri(R, 28);
                }
                if (bt == BT_FILL && i >= 12) { note = rootm + (i == 12 ? 7 : i == 13 ? 5 : i == 14 ? 3 : 2); vel = 105; }
                mbar_note(&m, i, len, note, vel, stac);
            }
            if (intro && b == 0 && chance(R, 0.3)) m.n = 1;
            emit_mbar(&tr[4], &m);
        }
        /* ---- keys (clav) comp ---- */
        {
            MBar m; m.n = 0;
            if (!intro) for (int i = 0; i < 16; i++) if (cmask[i]) mbar_chord(&m, i, 1, cn, 4, 70 + ri(R, 24), true);
            emit_mbar(&tr[5], &m);
        }
        /* ---- chicken-scratch guitar ---- */
        {
            MBar m; m.n = 0; int top = cn[3] + 12 > 88 ? cn[3] : cn[3] + 12;
            if (!intro || b == 1) for (int i = 0; i < 16; i++) {
                bool hit = (i % 2 == 0) ? chance(R, 0.75) : chance(R, 0.55 * I);
                if (hit) mbar_note(&m, i, 1, (i % 4 == 2) ? top : top - 3 - (ch->quality == 0 ? 0 : 1), (i % 4 == 2) ? 88 : 46 + ri(R, 20), true);
            }
            emit_mbar(&tr[6], &m);
        }
        /* ---- horn stabs on alternating bars ---- */
        {
            MBar m; m.n = 0;
            if (b >= 4 && b % 2 == 1 && bt != BT_FILL) {
                int hn[3] = { cn[1] - 12 + 12, cn[2], cn[3] };       /* 3rd, 5th, 7th */
                mbar_chord(&m, horn_c, 1, hn, 3, 104, true);
                mbar_chord(&m, horn_a, 2, hn, 3, 108, true);
                if (chance(R, 0.5 + 0.3 * I)) mbar_chord(&m, horn_b, 1, hn, 3, 100, true);
            }
            emit_mbar(&tr[7], &m);
        }
        /* ---- lead lick every 4th bar from bar 8 ---- */
        {
            MBar m; m.n = 0;
            if (b >= 8 && b % 4 == 3 && bt != BT_FILL) {
                static const int PN[6] = { 0, 3, 5, 7, 10, 12 };
                int base = midi_in(g->root_pc, 67), pos = 0;
                static const int ST[6] = { 0, 3, 6, 8, 10, 12 };
                for (int i = 0; i < 6; i++) { pos = clampi(pos + ri(R, 3) - 1, 0, 5); mbar_note(&m, ST[i], i == 5 ? 4 : 2, base + PN[pos], 92, i < 5); }
            }
            emit_mbar(&tr[8], &m);
        }
    }
}

/* ============================================================ assembly */

static void gen_text(SB *out, const GOpt *g) {
    MnRng R = { g->seed * 0x9E3779B97F4A7C15ull + 0xC0FFEEull };
    for (int i = 0; i < 5; i++) mn_rng_f(&R);
    SB tr[12]; memset(tr, 0, sizeof tr);
    if (g->kind == G_FUNK) gen_funk(tr, &R, g); else gen_phonk(tr, &R, g);
    sb_add(out, "title %s (seed %llu)\ntempo %g\ntime 4/4\n", STYLES[g->kind], (unsigned long long)g->seed, g->bpm);
    if (g->kind == G_FUNK) {
        if (g->swing16 > 0) sb_add(out, "swing16=%g\n", g->swing16);
        sb_add(out, "track kick drums vol=1.0\n%s\n", tr[0].s);
        sb_add(out, "track snare drums vol=0.85 reverb=0.12 pan=-0.05\n%s\n", tr[1].s);
        sb_add(out, "track hats drums vol=0.5 pan=0.2\n%s\n", tr[2].s);
        sb_add(out, "track openhat drums vol=0.5 pan=0.2\n%s\n", tr[9].s);
        sb_add(out, "track perc drums vol=0.35 pan=-0.25\n%s\n", tr[3].s);
        sb_add(out, "track bass slap vol=0.95\n%s\n", tr[4].s);
        sb_add(out, "track keys clav vol=0.85 pan=-0.3 reverb=0.12\n%s\n", tr[5].s);
        sb_add(out, "track guitar wah vol=0.7 pan=0.35 reverb=0.1\n%s\n", tr[6].s);
        sb_add(out, "track horns stab vol=0.85 pan=0.1 reverb=0.2\n%s\n", tr[7].s);
        sb_add(out, "track lead brass vol=0.7 pan=-0.1 reverb=0.2 delay=0.12\n%s\n", tr[8].s);
    } else {
        sb_add(out, "track kick drums vol=1.0\n%s\n", tr[0].s);
        sb_add(out, "track snare drums vol=0.85 reverb=0.2\n%s\n", tr[1].s);
        sb_add(out, "track snap drums vol=0.6 reverb=0.25 pan=0.1\n%s\n", tr[8].s);
        sb_add(out, "track hats drums vol=0.6 pan=0.2\n%s\n", tr[2].s);
        sb_add(out, "track cowbells drums vol=0.6 pan=-0.25 delay=0.15 delay_time=0.75\n%s\n", tr[3].s);
        sb_add(out, "track bass 808 vol=0.72 glide=%g\n%s\n", g->kind == G_DRIFT ? 0.07 : 0.09, tr[4].s);
        sb_add(out, "track lead cowbell vol=0.9 pan=-0.1 reverb=0.3 delay=0.22 delay_time=0.75\n%s\n", tr[5].s);
        sb_add(out, "track bells bell vol=0.5 pan=0.3 reverb=0.5 delay=0.3 delay_time=1.5\n%s\n", tr[6].s);
        sb_add(out, "track pad pad vol=0.4 reverb=0.6 lowpass=3500\n%s\n", tr[7].s);
    }
    for (int i = 0; i < 12; i++) sb_free(&tr[i]);
}

/* tracks that are ducked by the kick when producing */
static bool ducked(const char *name) {
    static const char *D[] = { "bass", "lead", "bells", "pad", "keys" };
    for (size_t i = 0; i < sizeof D / sizeof D[0]; i++) if (!strcmp(name, D[i])) return true;
    return false;
}

/* groove(style[, options]) -> notation text */
static KcValue gr_groove(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "groove(style[, \"bars=16 bpm key seed intensity swing scale\"]) -> notation text   (styles: phonk, drift_phonk, brazilian_phonk, funk)");
    GOpt g; parse_gopt(a[0], n > 1 ? a[1] : kc_null(), &g, pos, "groove");
    SB out = { 0 }; gen_text(&out, &g);
    KcValue v = kc_string(out.s); sb_free(&out);
    return v;
}
static KcValue gr_styles(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    KcValue out = kc_array_new();
    for (size_t i = 0; i < sizeof STYLES / sizeof STYLES[0]; i++) kc_array_push(out, kc_string(STYLES[i]));
    return out;
}

/* render every track of the generated score on its own */
typedef struct { char name[40]; MnAudio *au; } Stem;
static Stem *build_stems(const GOpt *g, KcValue o, int *count, KcPos pos, const char *who) {
    SB text = { 0 }; gen_text(&text, g);
    MnScore *S = mn_score_parse(text.s, text.n, false, pos, who); sb_free(&text);
    Stem *st = kc_malloc((size_t)S->ntr * sizeof *st); int k = 0;
    for (int i = 0; i < S->ntr; i++) {
        if (S->tr[i].n == 0) continue;
        KcValue ro = kc_object_new();
        SETK(ro, "solo", kc_string(S->tr[i].name));
        SETK(ro, "rate", kc_int(mn_opt_int(o, "rate", MN_RATE, pos, who)));
        SETK(ro, "tail", kc_float(mn_opt_num(o, "tail", 2.0, pos, who)));
        SETK(ro, "humanize", kc_float(mn_opt_num(o, "humanize", g->kind == G_FUNK ? 6.0 : 3.0, pos, who)));
        SETK(ro, "seed", kc_int((int64_t)g->seed));
        snprintf(st[k].name, sizeof st[k].name, "%s", S->tr[i].name);
        st[k].au = mn_score_render(S, ro, pos, who);
        k++;
    }
    mn_score_free(S); *count = k;
    return st;
}
static MnAudio *sum_stems(Stem *st, int n, bool duck, double depth, KcPos pos, const char *who) {
    const MnAudio *key = NULL; size_t maxf = 0; int rate = MN_RATE;
    for (int i = 0; i < n; i++) { if (!strcmp(st[i].name, "kick")) key = st[i].au; if (st[i].au->frames > maxf) maxf = st[i].au->frames; rate = st[i].au->rate; }
    MnAudio *mix = mn_audio_new(maxf, 2, rate, pos, who);
    for (int i = 0; i < n; i++) {
        MnAudio *s = st[i].au;
        if (duck && key && ducked(st[i].name)) mn_sidechain(s, key, depth, 0.35, 0.004, 0.16);
        for (size_t f = 0; f < s->frames * 2; f++) mix->d[f] += s->d[f] * 0.62f;
    }
    return mix;
}

/* produce(style[, options]) -> audio. options: groove options + master=phonk|funk|clean|loud|lofi|tape|none, sidechain=0.55, rate, tail, humanize */
static KcValue gr_produce(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "produce(style[, \"bars=16 bpm key seed intensity master=phonk sidechain=0.55 rate tail humanize\"]) -> audio");
    KcValue o = n > 1 ? a[1] : kc_null();
    GOpt g; parse_gopt(a[0], o, &g, pos, "produce");
    int ns; Stem *st = build_stems(&g, o, &ns, pos, "produce");
    double sc = mn_opt_num(o, "sidechain", g.kind == G_FUNK ? 0.0 : 0.55, pos, "produce");
    MnAudio *mix = sum_stems(st, ns, sc > 0, sc, pos, "produce");
    for (int i = 0; i < ns; i++) mn_audio_free(st[i].au);
    free(st);
    const char *mp = mn_opt_str(o, "master", g.kind == G_FUNK ? "funk" : "phonk", pos, "produce");
    if (strcasecmp(mp, "none")) { MnAudio *m = mn_master(mix, mp, pos, "produce"); mn_audio_free(mix); mix = m; }
    return mn_audio_wrap(mix);
}
/* stems(style[, options]) -> [{name, audio, ducked}] — every track rendered on its own */
static KcValue gr_stems(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "stems(style[, options]) -> [{name, audio, ducked}]");
    KcValue o = n > 1 ? a[1] : kc_null();
    GOpt g; parse_gopt(a[0], o, &g, pos, "stems");
    int ns; Stem *st = build_stems(&g, o, &ns, pos, "stems");
    KcValue out = kc_array_new();
    for (int i = 0; i < ns; i++) {
        KcValue e = kc_object_new();
        SETK(e, "name", kc_string(st[i].name)); SETK(e, "audio", mn_audio_wrap(st[i].au)); SETK(e, "ducked", kc_bool(ducked(st[i].name)));
        kc_array_push(out, e);
    }
    free(st);
    return out;
}

KcModuleEntry mn_groove_entries[] = {
    { "groove", gr_groove }, { "groove_styles", gr_styles }, { "produce", gr_produce }, { "stems", gr_stems },
};
const int mn_groove_count = (int)(sizeof mn_groove_entries / sizeof mn_groove_entries[0]);
