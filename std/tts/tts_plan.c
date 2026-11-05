/* KingCobra std.tts — utterance planning: phone units, durations, articulatory segments and the pitch contour.
 *  - weak/strong forms: grammatical words are reduced, content words carry the pitch accents
 *  - Klatt-style duration rules (stress, clusters, pre-voiceless shortening, phrase-final lengthening, rate)
 *  - US flapping, optional UK r-dropping
 *  - stops = closure + burst + aspiration, affricates, fricatives, nasals (anti-formant), liquids, diphthongs
 *  - intonation: declination, accents on stressed content words, falling statements, rising yes/no questions,
 *    continuation rises on commas, stronger accents for emphasis
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "tts_internal.h"
#include <math.h>

typedef struct {
    uint8_t kind;                /* 0 phone, 1 pause */
    uint8_t ph, st;
    int word, ip;
    bool first, last, weak, content, acr;
    float rate, vol, pit, pause_ms;
    int voice; uint8_t emph;
    int nsyl, pk, sty, ev;
    float amount;
} Unit;
typedef struct { char tune; bool yn; int nea; float ea_t[24], ea_v[24]; } IpInfo;

static bool weak_word(const char *w) {
    static const char *const W[] = { "the", "a", "an", "of", "to", "in", "on", "at", "for", "with", "and", "or", "but", "as", "by", "from", "than", "that", "is", "are", "was", "were", "be", "been", "am", "has", "have", "had",
        "do", "does", "did", "will", "would", "shall", "should", "can", "could", "may", "might", "must", "i", "you", "he", "she", "it", "we", "they", "me", "him", "her", "us", "them", "my", "your", "his", "its", "our", "their",
        "if", "into", "so", "than", "then", "there", "who", "whom", "whose", "which", "what", "when", "where", "because", "while", "until", "since", "per", "via", "upon", "onto", NULL };
    size_t L = strlen(w);
    if (L > 2 && (w[L - 2] == '\'' || (L > 3 && w[L - 3] == '\''))) return true;
    for (int i = 0; W[i]; i++) if (!strcmp(w, W[i])) return true;
    return false;
}
static bool is_aux(const char *w) {
    static const char *const A[] = { "is", "are", "am", "was", "were", "do", "does", "did", "can", "could", "will", "would", "should", "shall", "may", "might", "must", "have", "has", "had", "isn't", "aren't", "don't", "doesn't", "didn't", "can't", "won't", NULL };
    for (int i = 0; A[i]; i++) if (!strcmp(w, A[i])) return true;
    return false;
}
static bool is_wh(const char *w) {
    static const char *const A[] = { "who", "whom", "whose", "what", "which", "when", "where", "why", "how", NULL };
    for (int i = 0; A[i]; i++) if (!strcmp(w, A[i])) return true;
    return false;
}

static void seg_push(TtsPlan *pl, const TtsSeg *s) {
    if (pl->n == pl->cap) { pl->cap = pl->cap ? pl->cap * 2 : 256; pl->s = realloc(pl->s, (size_t)pl->cap * sizeof(TtsSeg)); }
    pl->s[pl->n++] = *s;
}
static TtsSeg seg_new(const Unit *u, int voice) {
    TtsSeg s; memset(&s, 0, sizeof s);
    s.word = u ? u->word : -1; s.voice = voice; s.sty = u ? u->sty : 0; if (u) { s.ph = u->ph; s.st = u->st; s.pk = u->pk; } s.vol = u ? u->vol : 1; s.pit = u ? u->pit : 0; s.ip = u ? u->ip : -1;
    s.bw[0] = 60; s.bw[1] = 90; s.bw[2] = 140; s.fc = 4000; s.fbw = 2000; s.tin = s.tout = 28;
    return s;
}
void tts_plan_free(TtsPlan *pl) { free(pl->sty); free(pl->s); free(pl->f0t); free(pl->f0v); free(pl->mark_seg); for (int i = 0; i < pl->nmark; i++) free(pl->mark_name[i]); free(pl->mark_name); memset(pl, 0, sizeof *pl); }

static int next_ph(const Unit *U, int n, int i) { return (i + 1 < n && U[i + 1].kind == 0) ? U[i + 1].ph : 0; }
static int prev_ph(const Unit *U, int n, int i) { (void)n; return (i > 0 && U[i - 1].kind == 0) ? U[i - 1].ph : 0; }
static bool voiceless_obs(int p) { return p == P_P || p == P_T || p == P_K || p == P_CH || p == P_F || p == P_TH || p == P_S || p == P_SH; }
static bool is_obstruent(int p) { return p >= P_B && p <= P_ZH && TTS_PH[p].cls >= C_STOP && TTS_PH[p].cls <= C_FRIC; }

static void add_f0(TtsPlan *pl, float t, float st) {
    if (pl->nf0 == pl->capf0) { pl->capf0 = pl->capf0 ? pl->capf0 * 2 : 128; pl->f0t = realloc(pl->f0t, (size_t)pl->capf0 * sizeof(float)); pl->f0v = realloc(pl->f0v, (size_t)pl->capf0 * sizeof(float)); }
    if (pl->nf0 && t < pl->f0t[pl->nf0 - 1]) t = pl->f0t[pl->nf0 - 1];
    pl->f0t[pl->nf0] = t; pl->f0v[pl->nf0] = st; pl->nf0++;
}


/* ================================================================== sound events: laugh, sigh, gasp, ... */
static int plan_style_add(TtsPlan *pl, const TtsStyle *st) {
    for (int i = 0; i < pl->nsty; i++) if (!memcmp(&pl->sty[i], st, sizeof *st)) return i;
    pl->sty = realloc(pl->sty, (size_t)(pl->nsty + 1) * sizeof(TtsStyle)); pl->sty[pl->nsty] = *st; return pl->nsty++;
}
typedef struct { TtsPlan *pl; IpInfo *ip; int ipi, sty, voice; float t; } Ev;
static void ev_f0(Ev *e, float dt, float st) { if (e->ip->nea < 24) { e->ip->ea_t[e->ip->nea] = e->t + dt; e->ip->ea_v[e->ip->nea++] = st; } }
static void ev_seg(Ev *e, float dur, const float *fS, const float *fE, float av0, float av1, float ah0, float ah1, float af0, float af1, float fc, float fbw, float nas, float fz, bool soft) {
    TtsSeg s; memset(&s, 0, sizeof s); s.voice = e->voice; s.sty = e->sty; s.word = -1; s.vol = 1; s.ip = e->ipi; s.dur = dur; s.soft = soft ? 1 : 0; s.tin = s.tout = 12;
    for (int k = 0; k < 3; k++) { s.fS[k] = fS[k]; s.fE[k] = fE ? fE[k] : fS[k]; }
    s.bw[0] = 80; s.bw[1] = 120; s.bw[2] = 170; s.avS = av0; s.avE = av1; s.ahS = ah0; s.ahE = ah1; s.afS = af0; s.afE = af1; s.fc = fc; s.fbw = fbw; s.nasal = nas; s.fz = fz;
    TtsPlan *pl = e->pl; if (pl->n == pl->cap) { pl->cap = pl->cap ? pl->cap * 2 : 256; pl->s = realloc(pl->s, (size_t)pl->cap * sizeof(TtsSeg)); }
    pl->s[pl->n++] = s; s.dur = dur; e->t += dur;
}
static void ev_gap(Ev *e, float dur) { static const float F[3] = { 500, 1500, 2500 }; ev_seg(e, dur, F, NULL, 0, 0, 0, 0, 0, 0, 4000, 2000, 0, 0, false); }
static void emit_event(TtsPlan *pl, IpInfo *ip, int ipi, int evid, float amt, int voice, int base_sty) {
    const char *name = tts_event_name(evid);
    if (amt < 0.4f) amt = 0.4f; if (amt > 2.5f) amt = 2.5f;
    TtsStyle es = pl->sty[base_sty < pl->nsty ? base_sty : 0];
    #define STY(...) do { __VA_ARGS__; } while (0)
    if (!strcmp(name, "laugh")) STY(es.breath += 0.12f, es.tilt -= 0.05f);
    else if (!strcmp(name, "giggle")) STY(es.breath += 0.1f, es.tilt -= 0.1f);
    else if (!strcmp(name, "chuckle")) STY(es.breath += 0.08f, es.tilt += 0.05f);
    else if (!strcmp(name, "cackle")) STY(es.tilt -= 0.25f, es.jitter += 0.02f, es.shimmer += 0.05f, es.oq -= 0.08f);
    else if (!strcmp(name, "sigh")) STY(es.breath += 0.25f, es.tilt += 0.15f);
    else if (!strcmp(name, "sob")) STY(es.tremor += 0.8f, es.tremor_hz = 6.0f, es.breath += 0.2f, es.jitter += 0.02f);
    else if (!strcmp(name, "cough")) STY(es.creak = 0.8f, es.jitter += 0.03f, es.tilt -= 0.1f);
    else if (!strcmp(name, "throat")) STY(es.creak = 0.9f, es.jitter += 0.03f);
    else if (!strcmp(name, "yawn")) STY(es.breath += 0.3f, es.tilt += 0.2f);
    else if (!strcmp(name, "scream")) STY(es.tilt -= 0.3f, es.oq -= 0.12f, es.tremor += 0.5f, es.tremor_hz = 7.0f, es.shimmer += 0.05f, es.vol *= 1.5f);
    else if (!strcmp(name, "groan")) STY(es.creak = 0.5f, es.breath += 0.1f);
    es.whisper = es.whisper;   /* whisper style whispers its laughs too */
    Ev e = { pl, ip, ipi, plan_style_add(pl, &es), voice, 0 };
    static const float AH[3] = { 720, 1220, 2450 }, AH2[3] = { 650, 1250, 2450 }, IH[3] = { 380, 2050, 2800 }, IH2[3] = { 420, 2000, 2700 }, EH[3] = { 540, 1750, 2500 };
    static const float UH[3] = { 560, 1300, 2450 }, MM[3] = { 250, 1100, 2200 }, MM2[3] = { 270, 1200, 2300 }, OPEN[3] = { 700, 1300, 2450 };
    ip->nea = 0;
    if (!strcmp(name, "laugh") || !strcmp(name, "cackle")) {
        bool ck = !strcmp(name, "cackle"); int n = (ck ? 5 : 3) + (int)(2.2f * amt); float T = ck ? 140.0f : 178.0f;
        for (int i = 0; i < n; i++) {
            float dec = (float)i / (float)n;
            float hd = ck ? 26 : 52, vd = (ck ? 92 : 100) - 6.0f * (float)i * (ck ? 0.3f : 1.0f); if (vd < 55) vd = 55;
            ev_f0(&e, 0, (ck ? 6.0f : 4.0f) - 0.8f * (float)i);
            ev_seg(&e, hd, AH, AH2, 34, 52 - 2.0f * (float)i, 50 - 5 * dec, 47 - 5 * dec, 0, 0, 4000, 2000, 0, 0, true);
            ev_f0(&e, 0, (ck ? 8.0f : 6.8f) - 0.95f * (float)i);
            ev_seg(&e, vd, AH2, AH, 58 - 2.2f * (float)i, 53 - 2.4f * (float)i, 40, 35, 0, 0, 4000, 2000, 0, 0, false);
            ev_f0(&e, 0, (ck ? 7.0f : 5.4f) - 1.0f * (float)i);
            float rest = T - hd - vd; if (rest > 8) ev_gap(&e, rest);
        }
        ev_seg(&e, 150, AH2, AH, 0, 0, 44, 14, 0, 0, 4000, 2000, 0, 0, true);
    } else if (!strcmp(name, "giggle")) {
        int n = 4 + (int)(2.0f * amt);
        for (int i = 0; i < n; i++) {
            ev_f0(&e, 0, 7.0f - 0.5f * (float)i);
            ev_seg(&e, 30, IH, IH2, 30, 50, 46, 43, 0, 0, 4000, 2000, 0, 0, true);
            ev_f0(&e, 0, 8.8f - 0.6f * (float)i);
            ev_seg(&e, 64, IH, IH2, 55 - 1.4f * (float)i, 50 - 1.4f * (float)i, 36, 32, 0, 0, 4000, 2000, 0, 0, false);
            ev_f0(&e, 0, 7.5f - 0.6f * (float)i);
            ev_gap(&e, 22);
        }
        ev_seg(&e, 100, IH2, IH, 0, 0, 40, 10, 0, 0, 4000, 2000, 0, 0, true);
    } else if (!strcmp(name, "chuckle")) {
        int n = 2 + (int)(1.6f * amt);
        for (int i = 0; i < n; i++) {
            ev_f0(&e, 0, -1.0f - 0.6f * (float)i);
            ev_seg(&e, 70, EH, EH, 28, 48, 44, 40, 0, 0, 4000, 2000, 0.25f, 1500, true);
            ev_f0(&e, 0, 0.4f - 0.9f * (float)i);
            ev_seg(&e, 115, EH, EH, 52 - 2.5f * (float)i, 47 - 2.5f * (float)i, 36, 30, 0, 0, 4000, 2000, 0.35f, 1500, false);
            ev_f0(&e, 0, -1.5f - 0.9f * (float)i);
            ev_gap(&e, 50);
        }
    } else if (!strcmp(name, "sigh")) {
        ev_seg(&e, 260 * amt, OPEN, OPEN, 0, 0, 34, 50, 0, 0, 4000, 2000, 0, 0, true);
        ev_gap(&e, 50);
        ev_f0(&e, 0, 2.0f);
        ev_seg(&e, 700 * amt, OPEN, UH, 44, 0, 52, 24, 0, 0, 4000, 2000, 0, 0, true);
        ev_f0(&e, 0, -5.0f);
    } else if (!strcmp(name, "gasp")) {
        ev_seg(&e, 230 * amt, OPEN, OPEN, 0, 0, 30, 58, 40, 50, 2500, 2500, 0, 0, true);
        ev_f0(&e, 0, 0);
        ev_seg(&e, 40, OPEN, OPEN, 30, 0, 44, 0, 0, 0, 4000, 2000, 0, 0, true);
    } else if (!strcmp(name, "sniff")) {
        for (int i = 0; i < 2; i++) { ev_seg(&e, 90, MM, MM2, 0, 0, 40, 0, 0, 52, 3000, 3000, 0.5f, 1000, true); ev_gap(&e, 70); }
    } else if (!strcmp(name, "sob")) {
        int n = 2 + (int)amt;
        for (int i = 0; i < n; i++) {
            ev_seg(&e, 130, OPEN, OPEN, 0, 0, 30, 50, 0, 0, 4000, 2000, 0, 0, true);
            ev_f0(&e, 0, 4.0f - 0.8f * (float)i);
            ev_seg(&e, 190, UH, UH, 54 - 2.0f * (float)i, 45, 40, 38, 0, 0, 4000, 2000, 0, 0, false);
            ev_f0(&e, 0, 1.0f - 0.8f * (float)i); ev_gap(&e, 60);
        }
        ev_seg(&e, 250, UH, UH, 0, 0, 46, 14, 0, 0, 4000, 2000, 0, 0, true);
    } else if (!strcmp(name, "cough")) {
        int n = 1 + (int)amt;
        for (int i = 0; i < n; i++) {
            ev_seg(&e, 50, UH, UH, 0, 30, 56, 52, 60, 54, 1700, 3000, 0, 0, false);
            ev_f0(&e, 0, -2.0f);
            ev_seg(&e, 85, UH, UH, 54, 40, 40, 30, 0, 0, 4000, 2000, 0, 0, false);
            ev_f0(&e, 0, -5.0f); ev_gap(&e, 140);
        }
    } else if (!strcmp(name, "throat")) {
        static const float K[3] = { 400, 1500, 2500 };
        ev_seg(&e, 160, K, K, 0, 0, 50, 46, 54, 50, 1500, 1200, 0, 0, false);
        ev_f0(&e, 0, -4.0f);
        ev_seg(&e, 130, UH, UH, 54, 46, 40, 36, 0, 0, 4000, 2000, 0, 0, false);
        ev_f0(&e, 0, -4.5f);
        ev_seg(&e, 260, MM, MM2, 54, 50, 0, 0, 0, 0, 4000, 2000, 1.0f, 1100, false);
        ev_f0(&e, 0, -2.0f);
    } else if (!strcmp(name, "hmm")) {
        float d = 420 * amt; ev_f0(&e, 0, 0.0f);
        ev_seg(&e, d, MM, MM2, 52, 52, 0, 0, 0, 0, 4000, 2000, 1.0f, 1100, false);
        ev_f0(&e, d * 0.4f, 2.0f); ev_f0(&e, d, -1.5f);
    } else if (!strcmp(name, "yawn")) {
        static const float O1[3] = { 500, 1250, 2450 }, O2[3] = { 850, 1300, 2450 }, O3[3] = { 450, 1200, 2400 };
        ev_f0(&e, 0, 1.0f);
        ev_seg(&e, 350 * amt, O1, O2, 0, 46, 40, 44, 0, 0, 4000, 2000, 0, 0, true);
        ev_f0(&e, 0, 3.0f);
        ev_seg(&e, 330 * amt, O2, O2, 46, 46, 44, 44, 0, 0, 4000, 2000, 0, 0, false);
        ev_seg(&e, 450 * amt, O2, O3, 46, 0, 44, 18, 0, 0, 4000, 2000, 0, 0, true);
        ev_f0(&e, 0, -7.0f);
    } else if (!strcmp(name, "scream")) {
        static const float S1[3] = { 850, 1300, 2600 };
        float d = 650 * (float)sqrt(amt); ev_f0(&e, 0, 9.0f);
        ev_seg(&e, d, S1, S1, 62, 60, 30, 26, 0, 0, 4000, 2000, 0, 0, false);
        ev_f0(&e, d * 0.3f, 12.0f); ev_f0(&e, d, 10.0f);
    } else if (!strcmp(name, "groan")) {
        static const float G1[3] = { 580, 1100, 2400 }, G2[3] = { 450, 1000, 2300 };
        float d = 800 * amt; ev_f0(&e, 0, -1.0f);
        ev_seg(&e, d, G1, G2, 54, 40, 38, 30, 0, 0, 4000, 2000, 0.2f, 1100, false);
        ev_f0(&e, d, -6.0f);
    } else if (!strcmp(name, "inhale")) {
        ev_seg(&e, 450 * amt, OPEN, OPEN, 0, 0, 24, 56, 0, 0, 4000, 2000, 0, 0, true);
    } else if (!strcmp(name, "exhale")) {
        ev_seg(&e, 550 * amt, OPEN, UH, 0, 0, 54, 18, 0, 0, 4000, 2000, 0, 0, true);
    } else if (!strcmp(name, "breath")) {
        ev_seg(&e, 380, OPEN, OPEN, 0, 0, 24, 54, 0, 0, 4000, 2000, 0, 0, true); ev_gap(&e, 100);
        ev_seg(&e, 500, OPEN, UH, 0, 0, 52, 16, 0, 0, 4000, 2000, 0, 0, true);
    }
    #undef STY
    if (ip->nea == 0) ev_f0(&e, 0, 0);
}

void tts_plan_build(const TtsWords *ws, const TtsOpts *o, TtsPlan *pl) {
    memset(pl, 0, sizeof *pl);
    if (ws->nsty > 0) { pl->nsty = ws->nsty; pl->sty = malloc((size_t)ws->nsty * sizeof(TtsStyle)); memcpy(pl->sty, ws->sty, (size_t)ws->nsty * sizeof(TtsStyle)); }
    else { pl->nsty = 1; pl->sty = malloc(sizeof(TtsStyle)); tts_style_neutral(&pl->sty[0]); }
    /* ---------------------------------------------------------- 1. flatten words to units */
    int cap = 256, nu = 0; Unit *U = malloc((size_t)cap * sizeof(Unit));
    int cap_ip = 16, nip = 0; IpInfo *IP = malloc((size_t)cap_ip * sizeof(IpInfo));
    int ip_units = 0; int sent_first = -1;
    #define UPUSH(u) do { if (nu == cap) { cap *= 2; U = realloc(U, (size_t)cap * sizeof(Unit)); } U[nu++] = (u); } while (0)
    for (int wi = 0; wi < ws->n; wi++) {
        const TtsWord *w = &ws->w[wi];
        if (w->kind == WK_MARK) {
            pl->mark_seg = realloc(pl->mark_seg, (size_t)(pl->nmark + 1) * sizeof(int)); pl->mark_name = realloc(pl->mark_name, (size_t)(pl->nmark + 1) * sizeof(char *));
            pl->mark_seg[pl->nmark] = -(nu + 1);          /* unit index for now; fixed up after segments exist */
            pl->mark_name[pl->nmark++] = strdup(w->text); continue;
        }
        if (w->kind == WK_WORD || w->kind == WK_PHONES) {
            if (w->nph <= 0) continue;
            if (sent_first < 0) sent_first = wi;
            bool weak = w->kind == WK_WORD && !w->emph && weak_word(w->text) && w->src != SRC_ACRONYM && w->src != SRC_SPELL;
            int nsyl = tts_syllables(w->ph, w->nph);
            for (int k = 0; k < w->nph; k++) {
                Unit u; memset(&u, 0, sizeof u); u.kind = 0; u.ph = w->ph[k].ph; u.st = w->ph[k].st; u.word = wi; u.ip = nip;
                u.first = k == 0; u.last = k == w->nph - 1; u.weak = weak; u.content = w->content; u.acr = (w->src == SRC_ACRONYM || w->src == SRC_SPELL);
                { int si = w->style >= 0 && w->style < pl->nsty ? w->style : 0; const TtsStyle *WS = &pl->sty[si]; u.sty = si; u.pk = k; u.rate = w->rate * WS->rate; u.vol = w->vol * WS->vol; u.pit = w->pitch_st + WS->pitch; } u.voice = w->voice; u.emph = w->emph; u.nsyl = nsyl;
                if (weak && u.st == 1) u.st = 0;
                if (weak && tts_is_vowel(u.ph) && u.ph == P_IY && !u.last) u.st = 0;
                UPUSH(u); ip_units++;
            }
            continue;
        }
        if (w->kind == WK_EVENT) {
            if (ip_units) { if (nip == cap_ip) { cap_ip *= 2; IP = realloc(IP, (size_t)cap_ip * sizeof(IpInfo)); } IP[nip].tune = ';'; IP[nip].yn = false; IP[nip].nea = 0; nip++; ip_units = 0; }
            if (nip == cap_ip) { cap_ip *= 2; IP = realloc(IP, (size_t)cap_ip * sizeof(IpInfo)); }
            IP[nip].tune = 'E'; IP[nip].yn = false; IP[nip].nea = 0;
            Unit e; memset(&e, 0, sizeof e); e.kind = 2; e.word = wi; e.ip = nip; e.ev = w->event; e.amount = w->amount > 0 ? w->amount : 1.0f; e.voice = w->voice; e.sty = w->style >= 0 && w->style < pl->nsty ? w->style : 0; e.rate = 1; e.vol = 1;
            UPUSH(e); nip++; continue;
        }
        if (w->kind == WK_PUNCT || w->kind == WK_BREAK) {
            if (ip_units) {
                if (nip == cap_ip) { cap_ip *= 2; IP = realloc(IP, (size_t)cap_ip * sizeof(IpInfo)); }
                IP[nip].tune = w->kind == WK_BREAK ? ',' : w->punc; IP[nip].yn = false;
                if (IP[nip].tune == '?') { const char *fw = sent_first >= 0 ? ws->w[sent_first].text : ""; IP[nip].yn = is_aux(fw) || !is_wh(fw); if (is_wh(fw)) IP[nip].yn = false; }
                nip++; ip_units = 0;
                if (w->kind == WK_PUNCT && (w->punc == '.' || w->punc == '?' || w->punc == '!' || w->punc == '~')) sent_first = -1;
            }
            Unit p; memset(&p, 0, sizeof p); p.kind = 1; p.word = -1; p.ip = -1; p.rate = 1; p.vol = 1;
            if (w->kind == WK_BREAK) { p.pause_ms = w->brk_ms; p.emph = 1; }
            else {
                double scale = o->pause_scale;
                switch (w->punc) { case ',': p.pause_ms = (float)(o->comma_ms * scale); break; case ';': case ':': p.pause_ms = (float)(o->comma_ms * 1.5 * scale); break;
                                   case '?': case '!': p.pause_ms = (float)(o->period_ms * 1.05 * scale); break; case '~': p.pause_ms = (float)(o->period_ms * 1.5 * scale); break; default: p.pause_ms = (float)(o->period_ms * scale); }
                if (w->para) p.pause_ms = (float)(o->para_ms * scale);
                if (w->kind == WK_PUNCT && (w->punc == ',' || w->punc == ';' || w->punc == ':')) p.pause_ms /= (float)pow(o->rate, 0.5);
                else if (w->kind == WK_PUNCT) p.pause_ms /= (float)pow(o->rate, 0.4);
                if (w->kind == WK_PUNCT) p.pause_ms *= pl->sty[w->style >= 0 && w->style < pl->nsty ? w->style : 0].pause;
            }
            UPUSH(p); continue;
        }
    }
    if (ip_units) { if (nip == cap_ip) { cap_ip *= 2; IP = realloc(IP, (size_t)cap_ip * sizeof(IpInfo)); } IP[nip].tune = '.'; IP[nip].yn = false; nip++; }
    if (nu > 0 && U[nu - 1].kind == 1 && !U[nu - 1].emph && U[nu - 1].pause_ms > 160) U[nu - 1].pause_ms = 160;   /* no long dead air at the very end */
    /* ---------------------------------------------------------- 2. phone modifications: flapping, UK r-dropping */
    for (int i = 1; i + 1 < nu; i++) {
        if (U[i].kind) continue;
        if (o->flap && !o->uk && (U[i].ph == P_T || U[i].ph == P_D) && U[i - 1].kind == 0 && tts_is_vowel(U[i - 1].ph) && U[i + 1].kind == 0 && tts_is_vowel(U[i + 1].ph) && U[i + 1].st == 0 && !U[i].first) U[i].ph = P_DX;
        if (o->flap && !o->uk && U[i].ph == P_T && U[i - 1].kind == 0 && U[i - 1].ph == P_R && U[i + 1].kind == 0 && tts_is_vowel(U[i + 1].ph) && U[i + 1].st == 0 && U[i - 1].st == 0) U[i].ph = P_DX;
    }
    if (o->uk) {
        for (int i = 0; i < nu; i++) {
            if (U[i].kind) continue;
            if (U[i].ph == P_R && i > 0 && U[i - 1].kind == 0 && tts_is_vowel(U[i - 1].ph) && !(i + 1 < nu && U[i + 1].kind == 0 && tts_is_vowel(U[i + 1].ph) && !U[i].last)) U[i].ph = P_NONE;
            else if (U[i].ph == P_ER) { U[i].ph = P_AH; if (U[i].st == 0) U[i].st = 0; }
        }
    }
    {   /* compact removed phones */
        int k = 0; for (int i = 0; i < nu; i++) if (!(U[i].kind == 0 && U[i].ph == P_NONE)) U[k++] = U[i];
        if (k != nu) { /* fix mark unit indices */ for (int m = 0; m < pl->nmark; m++) if (pl->mark_seg[m] < 0 && -pl->mark_seg[m] - 1 > k) pl->mark_seg[m] = -(k + 1); nu = k; }
    }
    /* ---------------------------------------------------------- 3. segments */
    const TtsVoice *V0 = tts_voice_at(o->voice);
    TtsSeg lead = seg_new(NULL, o->voice); lead.dur = o->trim ? 1.0f : 40.0f; lead.flags = SEGF_NOFORM; seg_push(pl, &lead);
    int *unit_first_seg = malloc((size_t)(nu + 1) * sizeof(int));
    /* last vowel index per ip, for phrase-final lengthening */
    int *ip_last_vowel = malloc((size_t)(nip + 1) * sizeof(int)); for (int j = 0; j <= nip; j++) ip_last_vowel[j] = -1;
    for (int i = 0; i < nu; i++) if (U[i].kind == 0 && tts_is_vowel(U[i].ph)) ip_last_vowel[U[i].ip] = i;
    /* a stop between S and a vowel is unaspirated; track the previous unit */
    for (int i = 0; i < nu; i++) {
        unit_first_seg[i] = pl->n;
        const Unit *u = &U[i];
        if (u->kind == 2) {
            int start = pl->n; emit_event(pl, &IP[u->ip], u->ip, u->ev, u->amount, u->voice >= 0 ? u->voice : o->voice, u->sty);
            for (int q = start; q < pl->n; q++) pl->s[q].word = u->word;
            continue;
        }
        if (u->kind == 1) { TtsSeg s = seg_new(NULL, o->voice); s.dur = u->pause_ms; s.flags = SEGF_NOFORM; if (s.dur > 0) seg_push(pl, &s); continue; }
        int voice = u->voice >= 0 ? u->voice : o->voice; const TtsVoice *V = tts_voice_at(voice);
        double rate = o->rate * V->rate * u->rate; if (rate < 0.25) rate = 0.25;
        const TtsPhone *P = &TTS_PH[u->ph];
        int nx = next_ph(U, nu, i), pv = prev_ph(U, nu, i);
        bool next_is_pause = (i + 1 >= nu) || U[i + 1].kind != 0;
        bool pre_final = false;                      /* inside the last syllable of the phrase */
        int lv = ip_last_vowel[u->ip]; if (lv >= 0 && i >= lv) { pre_final = next_is_pause || (i + 1 < nu && U[i + 1].ip == u->ip && 1); /* checked below */ }
        double fin = 1.0;
        if (lv >= 0 && i >= lv && u->ip >= 0 && u->ip < nip) { char t = IP[u->ip].tune; fin = (t == '.' || t == '!') ? 1.5 : (t == '?' || t == '~') ? 1.4 : 1.22; if (i > lv) fin = 1.0 + (fin - 1.0) * 0.6; }
        (void)pre_final;
        double d = P->dur;
        bool vowel = tts_is_vowel(u->ph);
        TtsSeg s = seg_new(u, voice);
        s.accent = 0;
        if (vowel) {
            double fs = u->st == 1 ? 1.0 : u->st == 2 ? 0.88 : (u->weak ? 0.5 : 0.62);
            d *= fs; if (u->emph) d *= 1.3; if (u->nsyl >= 3) d *= 0.92;
            if (nx && voiceless_obs(nx) && !(next_is_pause)) d *= 0.8; else if (nx && is_obstruent(nx) && TTS_PH[nx].voiced) d *= 1.08; else if (nx == P_N || nx == P_M || nx == P_NG) d *= 0.95;
            if (u->last && !next_is_pause) d *= 1.03;
            d *= fin; d /= rate; if (d < P->dmin * 0.6) d = P->dmin * 0.6;
            /* formant targets */
            float f[3] = { (float)P->f1, (float)P->f2, (float)P->f3 }, g[3] = { (float)P->g1, (float)P->g2, (float)P->g3 };
            static const float SCH[3] = { 500, 1450, 2500 };
            if (u->st == 0) {
                float r = (u->ph == P_AH) ? 1.0f : (u->ph == P_IY || u->ph == P_IH) ? 0.25f : (u->ph == P_ER) ? 0.15f : 0.45f;
                if (u->weak) r = r < 0.45f ? r + 0.15f : 0.8f;
                for (int k = 0; k < 3; k++) { f[k] += (SCH[k] - f[k]) * r; if (P->cls == C_DIPH) g[k] += (SCH[k] - g[k]) * r * 0.5f; }
            }
            if (u->ph == P_AH && u->st != 0) { /* STRUT */ }
            s.avS = s.avE = (float)(u->st == 1 ? 60 : u->st == 2 ? 58 : u->weak ? 53 : 56) + (u->emph ? 2 : 0);
            for (int k = 0; k < 3; k++) { s.fS[k] = f[k]; s.fE[k] = f[k]; s.bw[k] = (float)(k == 0 ? P->b1 : k == 1 ? P->b2 : P->b3); }
            bool accent = u->st == 1 && ((u->content && !u->weak) || u->emph || u->acr);
            s.accent = accent;
            if (P->cls == C_DIPH) {
                TtsSeg a = s, b2 = s; double da = d * 0.38, db = d - da;
                a.dur = (float)da; a.tout = 0; b2.dur = (float)db; b2.tin = 0; b2.accent = 0;
                for (int k = 0; k < 3; k++) { b2.fS[k] = f[k]; b2.fE[k] = g[k]; }
                int pc = pv ? TTS_PH[pv].cls : C_SIL; if (pc == C_STOP || pc == C_AFFR) a.tin = 0;
                seg_push(pl, &a); seg_push(pl, &b2);
            } else {
                s.dur = (float)d;
                int pc = pv ? TTS_PH[pv].cls : C_SIL; if (pc == C_STOP || pc == C_AFFR) s.tin = 0;
                seg_push(pl, &s);
            }
            continue;
        }
        /* ---- consonants ---- */
        double cf = 1.0;
        if ((pv && !tts_is_vowel(pv) && !u->first) || (nx && !tts_is_vowel(nx) && !u->last && U[i + 1].word == u->word)) cf *= 0.86;
        if (u->weak) cf *= 0.85; if (u->emph) cf *= 1.15;
        { double ar = pl->sty[u->sty].artic; cf *= 1.0 + (ar - 1.0) * 0.35; }
        if (next_is_pause) cf *= 1.0 + (fin - 1.0) * 0.5;
        double crate = pow(rate, 0.72);
        float nextF[3] = { 1500, 1500, 2500 }; bool next_vowel = false;
        for (int j = i + 1; j < nu && U[j].kind == 0 && j <= i + 2; j++) { const TtsPhone *N = &TTS_PH[U[j].ph]; if (tts_is_vowel(U[j].ph) || N->cls == C_LIQ || N->cls == C_GLIDE) { nextF[0] = N->f1; nextF[1] = N->f2; nextF[2] = N->f3; next_vowel = tts_is_vowel(U[j].ph); if (tts_is_vowel(U[j].ph) && U[j].st == 0) { for (int k = 0; k < 3; k++) { static const float SC[3] = { 500, 1450, 2500 }; nextF[k] += (SC[k] - nextF[k]) * 0.4f; } } break; } break; }
        float locus[3] = { (float)P->f1, (float)P->f2, (float)P->f3 };
        if (P->place == 4 && next_vowel) { locus[1] = nextF[1] * 0.35f + 1400.0f; locus[2] = nextF[2] * 0.92f; }
        switch (P->cls) {
        case C_STOP: case C_AFFR: {
            bool aff = P->cls == C_AFFR;
            if (u->ph == P_Q) { s.dur = (float)(P->dur * cf / crate); s.flags = 0; for (int k = 0; k < 3; k++) { s.fS[k] = s.fE[k] = locus[k]; } seg_push(pl, &s); break; }
            double clos = P->dur * cf / crate; if (clos < P->dmin * 0.6) clos = P->dmin * 0.6;
            if (u->first && pv == 0) clos *= 0.9;
            TtsSeg c = s; c.dur = (float)clos; c.tin = 6; c.tout = 0; c.avS = c.avE = (float)P->av; c.afS = c.afE = 0;
            for (int k = 0; k < 3; k++) { c.fS[k] = c.fE[k] = locus[k]; c.bw[k] = (float)(k == 0 ? 60 : 110 + 50 * k); }
            if (pv == P_S || (pv && TTS_PH[pv].cls == C_STOP)) c.dur *= 0.8f;
            if (P->voiced && P->av) { c.avS = c.avE = 38; }
            seg_push(pl, &c);
            double vot = P->vot * (u->weak ? 0.8 : 1.0) / pow(rate, 0.5);
            bool stressed_next = next_vowel && i + 1 < nu && U[i + 1].st == 1;
            if (!P->voiced) { if (pv == P_S) vot *= 0.35; else if (stressed_next) vot *= 1.25; else if (!next_vowel) vot *= 0.5; else vot *= 0.8; }
            if (aff) { vot = P->vot * cf / crate; }
            float fc = (float)P->fc, fbw = (float)P->fbw;
            if (P->place == 4) { fc = nextF[1] * 1.1f; if (fc < 1500) fc = 1500; if (fc > 3200) fc = 3200; }
            TtsSeg r = s; r.sub = 2; r.tin = 0; r.tout = next_vowel ? 0 : 20;
            if (aff) {
                r.dur = (float)vot; r.soft = 0; r.afS = (float)P->fa; r.afE = (float)P->fa - 4; r.fc = fc; r.fbw = fbw; r.avS = r.avE = (float)P->av; r.vmod = P->voiced ? 1 : 0;
                for (int k = 0; k < 3; k++) { r.fS[k] = locus[k]; r.fE[k] = locus[k]; }
                seg_push(pl, &r);
            } else {
                /* burst (10 ms) then aspiration/voicing onset, formants gliding from locus into the next sound */
                TtsSeg b = r; b.dur = 10; b.afS = b.afE = (float)P->fa; b.fc = fc; b.fbw = fbw; b.ahS = b.ahE = P->voiced ? 0 : 40; b.avS = b.avE = P->voiced ? 44 : 0;
                for (int k = 0; k < 3; k++) { b.fS[k] = locus[k]; b.fE[k] = locus[k] + (next_vowel ? (nextF[k] - locus[k]) * 0.35f : 0); }
                seg_push(pl, &b);
                if (vot > 4 && next_vowel) {
                    TtsSeg a = r; a.dur = (float)vot; a.afS = 0; a.afE = 0; a.fc = fc;
                    float asp = (float)(stressed_next ? 49 : 44);
                    if (P->voiced) { a.ahS = a.ahE = 0; a.avS = 46; a.avE = 56; } else { a.ahS = asp; a.ahE = asp - 3; a.avS = a.avE = 0; }
                    for (int k = 0; k < 3; k++) { a.fS[k] = b.fE[k]; a.fE[k] = nextF[k]; }
                    seg_push(pl, &a);
                }
            }
            break;
        }
        case C_FRIC: {
            double dd = P->dur * cf / crate; if (dd < P->dmin * 0.6) dd = P->dmin * 0.6;
            s.dur = (float)dd; s.soft = 1; s.avS = s.avE = (float)P->av; s.afS = s.afE = (float)P->fa; s.fc = (float)P->fc; s.fbw = (float)P->fbw; s.vmod = P->voiced ? 1 : 0;
            if (P->voiced && pv && !tts_is_vowel(pv) && TTS_PH[pv].voiced == 0) { s.avS = s.avE = 0; }
            for (int k = 0; k < 3; k++) { s.fS[k] = s.fE[k] = locus[k]; s.bw[k] = (float)(k == 0 ? P->b1 : k == 1 ? P->b2 : P->b3); }
            seg_push(pl, &s); break;
        }
        case C_ASP: {
            double dd = P->dur * cf / crate; if (dd < P->dmin * 0.6) dd = P->dmin * 0.6;
            s.dur = (float)dd; s.soft = 1; s.ahS = s.ahE = (float)P->fa;
            bool inter = pv && tts_is_vowel(pv) && next_vowel;
            if (inter) { s.avS = s.avE = 44; s.ahS = s.ahE = 42; }
            for (int k = 0; k < 3; k++) { s.fS[k] = s.fE[k] = nextF[k]; s.bw[k] = (float)(k == 0 ? 90 : 130 + 20 * k); }
            s.tin = 18; s.tout = 0; seg_push(pl, &s); break;
        }
        case C_NASAL: {
            double dd = P->dur * cf / crate; if (dd < P->dmin * 0.6) dd = P->dmin * 0.6;
            s.dur = (float)dd; s.avS = s.avE = (float)P->av; s.nasal = 1; s.fz = (float)P->fz;
            for (int k = 0; k < 3; k++) { s.fS[k] = s.fE[k] = (float)(k == 0 ? P->f1 : k == 1 ? P->f2 : P->f3); s.bw[k] = (float)(k == 0 ? P->b1 : k == 1 ? P->b2 : P->b3); }
            if (next_is_pause || (nx && !tts_is_vowel(nx) && TTS_PH[nx].place) ) { int pl_ = nx ? TTS_PH[nx].place : 0; if (pl_ == 1) s.fz = 1100; else if (pl_ == 4) s.fz = 2300; else if (pl_ == 2) s.fz = 1800; }
            seg_push(pl, &s); break;
        }
        case C_LIQ: case C_GLIDE: case C_FLAP: {
            double dd = P->dur * cf / crate; if (dd < P->dmin * 0.6) dd = P->dmin * 0.6;
            s.dur = (float)dd; s.avS = s.avE = (float)P->av;
            for (int k = 0; k < 3; k++) { s.fS[k] = s.fE[k] = (float)(k == 0 ? P->f1 : k == 1 ? P->f2 : P->f3); s.bw[k] = (float)(k == 0 ? P->b1 : k == 1 ? P->b2 : P->b3); }
            if (u->ph == P_L && pv && tts_is_vowel(pv) && !next_vowel) { s.fS[1] = s.fE[1] = 1000; s.fS[0] = s.fE[0] = 420; }   /* dark L */
            if (u->ph == P_R && pv && tts_is_vowel(pv) && !next_vowel) { s.tin = 38; }
            if (P->cls == C_FLAP) { s.tin = 6; s.tout = 6; s.avS = s.avE = 42; }
            if (u->st == 0 && u->weak) s.avS = s.avE = s.avS - 2;
            seg_push(pl, &s); break;
        }
        default: break;
        }
    }
    TtsSeg tail = seg_new(NULL, o->voice); tail.dur = o->trim ? 5.0f : 70.0f; tail.flags = SEGF_NOFORM; seg_push(pl, &tail);
    /* ---------------------------------------------------------- 4. times */
    float t = 0; for (int i = 0; i < pl->n; i++) { pl->s[i].t0 = t; t += pl->s[i].dur; } pl->total_ms = t;
    for (int m = 0; m < pl->nmark; m++) { int u = -pl->mark_seg[m] - 1; pl->mark_seg[m] = u >= 0 && u <= nu ? (u < nu ? unit_first_seg[u] : pl->n - 1) : pl->n - 1; }
    /* ---------------------------------------------------------- 5. formant ramps by context */
    for (int i = 0; i < pl->n; i++) {
        TtsSeg *s = &pl->s[i]; if (s->flags & SEGF_NOFORM) continue;
        float lim = s->dur * 0.45f; if (s->tin > lim) s->tin = lim; if (s->tout > lim) s->tout = lim;
        if (i > 0 && !(pl->s[i - 1].flags & SEGF_NOFORM)) { const TtsSeg *pv = &pl->s[i - 1]; if (s->ph == 0 && pv->avE == 0 && 0) {} (void)pv; }
    }
    /* ---------------------------------------------------------- 6. pitch contour */
    double rng0 = o->range * V0->range; if (V0->robot) rng0 = 0;
    for (int j = 0; j < nip; j++) {
        int s0 = -1, s1 = -1; for (int i = 0; i < pl->n; i++) if (pl->s[i].ip == j && !(pl->s[i].flags & SEGF_NOFORM)) { if (s0 < 0) s0 = i; s1 = i; }
        if (s0 < 0) continue;
        float ts = pl->s[s0].t0, te = pl->s[s1].t0 + pl->s[s1].dur; if (te - ts < 20) te = ts + 20;
        if (IP[j].tune == 'E') { const TtsStyle *ES = &pl->sty[pl->s[s0].sty]; for (int a = 0; a < IP[j].nea; a++) add_f0(pl, pl->s[s0].t0 + IP[j].ea_t[a], IP[j].ea_v[a] + ES->pitch); continue; }
        const TtsStyle *ST = &pl->sty[pl->s[s0].sty]; double rng = rng0 * ST->range;
        char tune = IP[j].tune; bool yn = IP[j].yn;
        float dec = (float)(1.6 * rng * ST->decl * (te - ts) / 1000.0); if (dec > 4.5f * (float)rng * (float)ST->decl) dec = 4.5f * (float)rng * (float)ST->decl;
        float top = (float)(2.2 * rng), bot = top - dec;
        if (tune == '!') top += (float)(1.2 * rng);
        if (tune == '?' && !yn) top += (float)(0.8 * rng);
        #define BL(tt) (top + (bot - top) * (((tt) - ts) / (te - ts)))
        /* accents */
        int na = 0; int acc_seg[256]; for (int i = s0; i <= s1 && na < 256; i++) if (pl->s[i].ip == j && pl->s[i].accent) acc_seg[na++] = i;
        add_f0(pl, ts, top + pl->s[s0].pit);
        float prev_t = ts; int k_acc = 0;
        for (int a = 0; a < na; a++) {
            TtsSeg *sg = &pl->s[acc_seg[a]]; float tc = sg->t0 + sg->dur * 0.45f;
            const TtsWord *wd = sg->word >= 0 ? &ws->w[sg->word] : NULL; float emph = (wd && wd->emph) ? (float)(wd->emph == 2 ? 2.0 : 1.0) : 0;
            const TtsStyle *AS = &pl->sty[sg->sty]; float lilt = 1.0f + AS->lilt * 0.45f * ((k_acc & 1) ? -1.0f : 1.0f);
            float peak = (float)((3.0 * rng) * pow(0.93, k_acc) * AS->accent * lilt) + (float)(emph * 1.2 * rng);
            bool last_acc = a == na - 1;
            if (last_acc) { if (tune == '?' && yn) peak = (float)(0.4 * rng); else if (tune == '!') peak += (float)(1.0 * rng); else if (tune == ',' || tune == ';' || tune == ':') peak *= 0.85f; else peak *= 1.05f; }
            if (a > 0 && tc - prev_t > 120) add_f0(pl, (prev_t + tc) * 0.5f, BL((prev_t + tc) * 0.5f) - (float)(0.6 * rng) + sg->pit);
            else if (a == 0 && tc - ts > 90) add_f0(pl, tc - (tc - ts) * 0.4f, BL(tc - (tc - ts) * 0.4f) - (float)(0.2 * rng) + sg->pit);
            add_f0(pl, tc, BL(tc) + peak + sg->pit);
            prev_t = tc; k_acc++;
        }
        /* terminal movement */
        float endv;
        switch (tune) {
        case '?': endv = yn ? (float)(top + 2.2 * rng) : (float)(bot - 1.8 * rng); break;
        case '!': endv = (float)(bot - 2.6 * rng * ST->fall); break;
        case '~': endv = (float)(bot - 0.4 * rng); break;
        case ',': endv = (float)(bot + 0.9 * rng); break;
        case ';': case ':': endv = (float)(bot + 0.2 * rng); break;
        default: endv = (float)(bot - 2.0 * rng * ST->fall); break;
        }
        if (yn && tune == '?' && na) { float tl = pl->s[acc_seg[na - 1]].t0 + pl->s[acc_seg[na - 1]].dur; float mid = tl + (te - tl) * 0.45f; if (te - tl > 100) add_f0(pl, mid, (float)(bot - 1.0 * rng)); }
        add_f0(pl, te, endv + pl->s[s1].pit);
        if (tune == '~') for (int i = s0; i <= s1; i++) if (pl->s[i].ip == j) { float tm = pl->s[i].t0 + pl->s[i].dur * 0.5f; if (tm > te - 350.0f) pl->s[i].vol *= 1.0f - 0.55f * (tm - (te - 350.0f)) / 350.0f; }
        #undef BL
    }
    free(U); free(IP); free(unit_first_seg); free(ip_last_vowel);
}
