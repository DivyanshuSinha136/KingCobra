/* KingCobra std.tts — phoneme inventory (ARPAbet + flap + glottal stop), articulatory parameters,
 * ARPAbet/IPA conversion and the built-in voices.
 * Formant values follow Peterson & Barney / Klatt (1980) averages for an adult male; other voices scale them.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "tts_internal.h"
#include <ctype.h>

/* fields: arpa ipa  cls voiced place viseme  dur dmin vot  f1 f2 f3  g1 g2 g3  b1 b2 b3  av  fc fbw fa  fz */
#define V(a, i, vis, d, dm, F1, F2, F3, B1, B2, B3) { a, i, C_VOWEL, 1, 0, vis, d, dm, 0, F1, F2, F3, 0, 0, 0, B1, B2, B3, 60, 0, 0, 0, 0 }
#define D(a, i, vis, d, dm, F1, F2, F3, G1, G2, G3) { a, i, C_DIPH, 1, 0, vis, d, dm, 0, F1, F2, F3, G1, G2, G3, 60, 95, 150, 60, 0, 0, 0, 0 }

const TtsPhone TTS_PH[P_COUNT] = {
    [P_NONE]  = { "", "", C_SIL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    [P_AA] = V("AA", "ɑ", 1, 130, 70, 730, 1100, 2450, 60, 90, 140),
    [P_AE] = V("AE", "æ", 2, 135, 70, 670, 1720, 2450, 60, 90, 140),
    [P_AH] = V("AH", "ʌ", 6, 90, 45, 640, 1240, 2450, 60, 90, 140),
    [P_AO] = V("AO", "ɔ", 4, 130, 70, 590, 880, 2550, 60, 90, 140),
    [P_AW] = D("AW", "aʊ", 1, 175, 95, 700, 1250, 2450, 450, 950, 2300),
    [P_AY] = D("AY", "aɪ", 1, 175, 95, 700, 1250, 2450, 400, 1980, 2600),
    [P_EH] = V("EH", "ɛ", 2, 100, 50, 540, 1760, 2500, 60, 90, 140),
    [P_ER] = V("ER", "ɝ", 13, 125, 65, 480, 1350, 1690, 70, 100, 130),
    [P_EY] = D("EY", "eɪ", 3, 150, 80, 480, 1950, 2600, 330, 2250, 2950),
    [P_IH] = V("IH", "ɪ", 3, 85, 45, 400, 1960, 2550, 60, 90, 140),
    [P_IY] = V("IY", "i", 3, 120, 65, 290, 2250, 3000, 50, 90, 150),
    [P_OW] = D("OW", "oʊ", 14, 150, 80, 510, 950, 2400, 350, 850, 2250),
    [P_OY] = D("OY", "ɔɪ", 4, 190, 100, 560, 900, 2450, 400, 1980, 2600),
    [P_UH] = V("UH", "ʊ", 5, 85, 45, 450, 1050, 2350, 60, 90, 140),
    [P_UW] = V("UW", "u", 5, 125, 65, 340, 1050, 2200, 55, 90, 140),

    /* stops: dur = closure, vot = aspiration; formant fields are the consonant locus */
    [P_B]  = { "B",  "b",  C_STOP, 1, 1, 7,  50, 30, 8,   200, 900, 2300, 0, 0, 0, 60, 110, 160, 38, 1300, 1800, 46, 0 },
    [P_P]  = { "P",  "p",  C_STOP, 0, 1, 7,  58, 36, 38,  200, 900, 2300, 0, 0, 0, 60, 110, 160, 0,  1400, 1900, 50, 0 },
    [P_D]  = { "D",  "d",  C_STOP, 1, 2, 10, 45, 28, 10,  200, 1750, 2850, 0, 0, 0, 60, 110, 160, 38, 3900, 2200, 51, 0 },
    [P_T]  = { "T",  "t",  C_STOP, 0, 2, 10, 56, 34, 42,  200, 1750, 2850, 0, 0, 0, 60, 110, 160, 0,  4300, 2400, 58, 0 },
    [P_G]  = { "G",  "ɡ",  C_STOP, 1, 4, 12, 50, 30, 14,  200, 1650, 2500, 0, 0, 0, 60, 110, 160, 38, 2300, 1000, 51, 0 },
    [P_K]  = { "K",  "k",  C_STOP, 0, 4, 12, 62, 38, 48,  200, 1650, 2500, 0, 0, 0, 60, 110, 160, 0,  2300, 1000, 58, 0 },
    [P_CH] = { "CH", "tʃ", C_AFFR, 0, 3, 11, 45, 30, 85,  250, 1800, 2600, 0, 0, 0, 60, 110, 160, 0,  3300, 2000, 58, 0 },
    [P_JH] = { "JH", "dʒ", C_AFFR, 1, 3, 11, 40, 26, 70,  250, 1800, 2600, 0, 0, 0, 60, 110, 160, 40, 3000, 1900, 51, 0 },

    /* fricatives */
    [P_F]  = { "F",  "f",  C_FRIC, 0, 1, 8,  100, 60, 0,  250, 1100, 2400, 0, 0, 0, 80, 150, 200, 0,  5200, 6500, 38, 0 },
    [P_V]  = { "V",  "v",  C_FRIC, 1, 1, 8,  65,  35, 0,  250, 1100, 2400, 0, 0, 0, 80, 150, 200, 46, 5200, 6500, 30, 0 },
    [P_TH] = { "TH", "θ",  C_FRIC, 0, 6, 9,  90,  55, 0,  250, 1400, 2500, 0, 0, 0, 80, 150, 200, 0,  5200, 6500, 33, 0 },
    [P_DH] = { "DH", "ð",  C_FRIC, 1, 6, 9,  45,  25, 0,  250, 1400, 2500, 0, 0, 0, 80, 150, 200, 46, 5200, 6500, 27, 0 },
    [P_S]  = { "S",  "s",  C_FRIC, 0, 2, 10, 110, 65, 0,  250, 1700, 2800, 0, 0, 0, 80, 150, 200, 0,  6400, 3000, 57, 0 },
    [P_Z]  = { "Z",  "z",  C_FRIC, 1, 2, 10, 85,  45, 0,  250, 1700, 2800, 0, 0, 0, 80, 150, 200, 46, 6200, 3000, 50, 0 },
    [P_SH] = { "SH", "ʃ",  C_FRIC, 0, 3, 11, 115, 70, 0,  250, 1800, 2400, 0, 0, 0, 80, 150, 200, 0,  3300, 1900, 58, 0 },
    [P_ZH] = { "ZH", "ʒ",  C_FRIC, 1, 3, 11, 90,  50, 0,  250, 1800, 2400, 0, 0, 0, 80, 150, 200, 46, 3100, 1900, 50, 0 },
    [P_HH] = { "HH", "h",  C_ASP,  0, 5, 6,  70,  35, 0,  500, 1500, 2500, 0, 0, 0, 100, 150, 200, 0, 0, 0, 52, 0 },

    /* sonorants */
    [P_M]  = { "M",  "m",  C_NASAL, 1, 1, 7,  75, 45, 0,  250, 1000, 2200, 0, 0, 0, 90, 160, 220, 49, 0, 0, 0, 1100 },
    [P_N]  = { "N",  "n",  C_NASAL, 1, 2, 10, 65, 40, 0,  250, 1650, 2600, 0, 0, 0, 90, 160, 220, 50, 0, 0, 0, 1800 },
    [P_NG] = { "NG", "ŋ",  C_NASAL, 1, 4, 12, 85, 50, 0,  250, 1950, 2600, 0, 0, 0, 90, 160, 220, 50, 0, 0, 0, 2300 },
    [P_L]  = { "L",  "l",  C_LIQ,   1, 2, 10, 70, 40, 0,  390, 1200, 2650, 0, 0, 0, 70, 120, 170, 57, 0, 0, 0, 0 },
    [P_R]  = { "R",  "ɹ",  C_LIQ,   1, 0, 13, 75, 40, 0,  350, 1100, 1500, 0, 0, 0, 70, 110, 140, 57, 0, 0, 0, 0 },
    [P_W]  = { "W",  "w",  C_GLIDE, 1, 1, 14, 70, 35, 0,  300, 650, 2200,  0, 0, 0, 70, 110, 160, 56, 0, 0, 0, 0 },
    [P_Y]  = { "Y",  "j",  C_GLIDE, 1, 3, 15, 65, 30, 0,  270, 2150, 3050, 0, 0, 0, 60, 100, 160, 56, 0, 0, 0, 0 },
    [P_DX] = { "DX", "ɾ",  C_FLAP,  1, 2, 10, 26, 14, 0,  250, 1700, 2800, 0, 0, 0, 80, 140, 200, 48, 0, 0, 0, 0 },
    [P_Q]  = { "Q",  "ʔ",  C_STOP,  0, 5, 0,  38, 25, 0,  400, 1400, 2500, 0, 0, 0, 80, 140, 200, 0, 0, 0, 0, 0 },
    [P_PAUSE] = { "_", "", C_SIL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

int tts_ph_find(const char *name) {
    for (int i = 1; i < P_PAUSE; i++) if (!strcasecmp(name, TTS_PH[i].arpa)) return i;
    return 0;
}

int tts_phseq_parse_ex(const char *s, TtsPh *out, int cap, char *bad, size_t badn, int def_stress) {
    int n = 0;
    while (*s) {
        while (*s && (isspace((unsigned char)*s) || *s == '|' || *s == '-' || *s == '.')) s++;
        if (!*s) break;
        char tok[16]; int k = 0;
        while (*s && !isspace((unsigned char)*s) && *s != '|' && *s != '-' && k < 15) tok[k++] = *s++;
        tok[k] = 0;
        int st = ST_UNSET;
        if (k > 1 && isdigit((unsigned char)tok[k - 1])) { st = tok[k - 1] - '0'; if (st > 2) st = 2; tok[--k] = 0; }
        else if (k > 1 && (tok[k - 1] == '\'' )) { st = 1; tok[--k] = 0; }
        int p = tts_ph_find(tok);
        if (!p && !strcasecmp(tok, "AX")) { p = P_AH; st = 0; }       /* schwa spellings */
        else if (!p && !strcasecmp(tok, "AXR")) { p = P_ER; st = 0; }
        else if (!p && !strcasecmp(tok, "IX")) { p = P_IH; st = 0; }
        else if (!p && !strcasecmp(tok, "UX")) { p = P_UW; st = 0; }
        if (!p) { if (bad) snprintf(bad, badn, "%s", tok); return -1; }
        if (n >= cap) break;
        if (!tts_is_vowel(p)) st = 0;
        else if (st == ST_UNSET) st = def_stress;   /* no digit given: caller decides (user text: stressed; G2P rules: decide later) */
        out[n].ph = (uint8_t)p; out[n].st = (uint8_t)st; n++;
    }
    return n;
}

int tts_phseq_parse(const char *s, TtsPh *out, int cap, char *bad, size_t badn) {
    return tts_phseq_parse_ex(s, out, cap, bad, badn, 1);
}

/* ---------------------------------------------------------------- IPA */
static const struct { const char *s; int ph; int st; } IPA_TAB[] = {
    { "tʃ", P_CH, 0 }, { "dʒ", P_JH, 0 }, { "aʊ", P_AW, -1 }, { "aɪ", P_AY, -1 }, { "eɪ", P_EY, -1 }, { "oʊ", P_OW, -1 }, { "ɔɪ", P_OY, -1 },
    { "ɑ", P_AA, -1 }, { "a", P_AA, -1 }, { "æ", P_AE, -1 }, { "ʌ", P_AH, -1 }, { "ə", P_AH, 0 }, { "ɔ", P_AO, -1 }, { "ɛ", P_EH, -1 }, { "ɝ", P_ER, -1 },
    { "ɚ", P_ER, 0 }, { "ɪ", P_IH, -1 }, { "i", P_IY, -1 }, { "ʊ", P_UH, -1 }, { "u", P_UW, -1 }, { "e", P_EY, -1 }, { "o", P_OW, -1 },
    { "b", P_B, 0 }, { "d", P_D, 0 }, { "ð", P_DH, 0 }, { "f", P_F, 0 }, { "ɡ", P_G, 0 }, { "g", P_G, 0 }, { "h", P_HH, 0 }, { "k", P_K, 0 },
    { "l", P_L, 0 }, { "m", P_M, 0 }, { "n", P_N, 0 }, { "ŋ", P_NG, 0 }, { "p", P_P, 0 }, { "ɹ", P_R, 0 }, { "r", P_R, 0 }, { "s", P_S, 0 },
    { "ʃ", P_SH, 0 }, { "t", P_T, 0 }, { "θ", P_TH, 0 }, { "v", P_V, 0 }, { "w", P_W, 0 }, { "j", P_Y, 0 }, { "z", P_Z, 0 }, { "ʒ", P_ZH, 0 },
    { "ɾ", P_DX, 0 }, { "ʔ", P_Q, 0 },
};

int tts_ipa_parse(const char *s, TtsPh *out, int cap) {
    int n = 0, pend = -1, nstress = 0, nvow = 0;
    while (*s) {
        if (isspace((unsigned char)*s) || *s == '.' || *s == '/' || *s == '[' || *s == ']') { s++; continue; }
        if (!strncmp(s, "ˈ", 2)) { pend = 1; nstress++; s += 2; continue; }
        if (!strncmp(s, "ˌ", 2)) { pend = 2; nstress++; s += 2; continue; }
        if (!strncmp(s, "ː", 2)) { s += 2; continue; }
        bool hit = false;
        for (size_t i = 0; i < sizeof IPA_TAB / sizeof IPA_TAB[0]; i++) {
            size_t L = strlen(IPA_TAB[i].s);
            if (strncmp(s, IPA_TAB[i].s, L)) continue;
            if (n >= cap) return n;
            int ph = IPA_TAB[i].ph, st = IPA_TAB[i].st;
            if (tts_is_vowel(ph)) {
                nvow++;
                if (st < 0) st = pend >= 0 ? pend : 0;
                else if (st == 0 && pend >= 0) st = pend;
                pend = -1;
            } else st = 0;
            out[n].ph = (uint8_t)ph; out[n].st = (uint8_t)st; n++;
            s += L; hit = true; break;
        }
        if (!hit) return -1;
    }
    if (nstress == 0 && nvow == 1) for (int i = 0; i < n; i++) if (tts_is_vowel(out[i].ph)) out[i].st = 1;
    return n;
}

char *tts_phseq_format(const TtsPh *p, int n, bool ipa, bool stress_digits) {
    TBuf b; tb_init(&b);
    if (!ipa) {
        for (int i = 0; i < n; i++) {
            if (i) tb_putc(&b, ' ');
            tb_puts(&b, TTS_PH[p[i].ph].arpa);
            if (stress_digits && tts_is_vowel(p[i].ph)) tb_putc(&b, (char)('0' + (p[i].st > 2 ? 0 : p[i].st)));
        }
        return tb_take(&b);
    }
    /* IPA: precompute where each stress mark goes (before the syllable onset), then emit */
    int *mark = calloc((size_t)n + 1, sizeof(int));
    for (int i = 0; i < n; i++) {
        if (!(tts_is_vowel(p[i].ph) && (p[i].st == 1 || p[i].st == 2))) continue;
        int j = i, cnt = 0;
        while (j > 0 && !tts_is_vowel(p[j - 1].ph) && cnt < 3) { j--; cnt++; }
        if (cnt == 3 && p[j].ph != P_S) { j++; cnt--; }
        if (cnt >= 2 && p[j].ph != P_S) { int a = p[j].ph, c2 = p[j + 1].ph; bool ok = (a == P_P || a == P_B || a == P_T || a == P_D || a == P_K || a == P_G || a == P_F) && (c2 == P_R || c2 == P_L || c2 == P_Y || c2 == P_W); if (!ok) j++; }
        mark[j] = p[i].st;
    }
    for (int i = 0; i < n; i++) {
        if (mark[i] == 1) tb_puts(&b, "ˈ"); else if (mark[i] == 2) tb_puts(&b, "ˌ");
        int ph = p[i].ph;
        if (ph == P_AH && p[i].st == 0) tb_puts(&b, "ə");
        else if (ph == P_ER && p[i].st == 0) tb_puts(&b, "ɚ");
        else tb_puts(&b, TTS_PH[ph].ipa);
    }
    free(mark);
    return tb_take(&b);
}

/* ---------------------------------------------------------------- voices */
static TtsVoice VOICES[32] = {
    /* name        description                       f0   range tract oq   tilt  breath jit    shim  rate  bw   whisper robot */
    { "male",     "adult male, neutral",             112, 1.00, 1.00, 0.60, 0.30, 0.04, 0.010, 0.04, 1.00, 1.0, false, false },
    { "female",   "adult female, neutral",           205, 1.15, 1.17, 0.65, 0.35, 0.07, 0.008, 0.04, 1.00, 1.0, false, false },
    { "child",    "child",                           275, 1.20, 1.40, 0.70, 0.40, 0.06, 0.010, 0.05, 1.05, 1.1, false, false },
    { "teen",     "teenager",                        150, 1.10, 1.10, 0.62, 0.32, 0.05, 0.010, 0.04, 1.05, 1.0, false, false },
    { "deep",     "deep, slow bass voice",            82, 0.80, 0.88, 0.55, 0.25, 0.03, 0.010, 0.04, 0.92, 0.9, false, false },
    { "elder",    "older speaker, breathy, slower",  100, 0.85, 1.02, 0.70, 0.45, 0.14, 0.025, 0.09, 0.88, 1.2, false, false },
    { "narrator", "warm male with a wide pitch range", 105, 1.35, 0.97, 0.58, 0.28, 0.04, 0.008, 0.03, 0.92, 1.0, false, false },
    { "whisper",  "whispered speech (no voicing)",   112, 0.60, 1.00, 0.60, 0.30, 0.00, 0.000, 0.00, 0.95, 1.0, true,  false },
    { "robot",    "monotone synthetic voice",        105, 0.00, 1.00, 0.45, 0.15, 0.00, 0.000, 0.00, 1.00, 0.8, false, true  },
    { "alien",    "high, wide-tracted, wobbly",      190, 1.50, 1.55, 0.50, 0.20, 0.02, 0.020, 0.06, 1.10, 0.9, false, false },
};
static int NVOICES = 10;

int tts_voice_count(void) { return NVOICES; }
TtsVoice *tts_voice_at(int i) { return (i >= 0 && i < NVOICES) ? &VOICES[i] : &VOICES[0]; }
int tts_voice_find(const char *name) {
    for (int i = 0; i < NVOICES; i++) if (!strcasecmp(VOICES[i].name, name)) return i;
    if (!strcasecmp(name, "man") || !strcasecmp(name, "default") || !strcasecmp(name, "adult")) return 0;
    if (!strcasecmp(name, "woman")) return 1;
    if (!strcasecmp(name, "kid")) return 2;
    return -1;
}
int tts_voice_define(const char *name, const char *base, KcValue opts, KcPos pos) {
    int bi = tts_voice_find(base ? base : "male");
    if (bi < 0) kc_error_at(pos, "tts.define_voice: unknown base voice '%s'", base);
    TtsVoice v = VOICES[bi];
    snprintf(v.name, sizeof v.name, "%s", name);
    snprintf(v.desc, sizeof v.desc, "custom voice based on %s", VOICES[bi].name);
    const char *w = "tts.define_voice";
    v.f0 = mn_opt_num(opts, "f0", v.f0, pos, w);
    v.range = mn_opt_num(opts, "range", v.range, pos, w);
    v.tract = mn_opt_num(opts, "tract", v.tract, pos, w);
    v.oq = mn_opt_num(opts, "oq", v.oq, pos, w);
    v.tilt = mn_opt_num(opts, "tilt", v.tilt, pos, w);
    v.breath = mn_opt_num(opts, "breath", v.breath, pos, w);
    v.jitter = mn_opt_num(opts, "jitter", v.jitter, pos, w);
    v.shimmer = mn_opt_num(opts, "shimmer", v.shimmer, pos, w);
    v.rate = mn_opt_num(opts, "rate", v.rate, pos, w);
    v.bw = mn_opt_num(opts, "bw", v.bw, pos, w);
    v.whisper = mn_opt_bool(opts, "whisper", v.whisper, pos, w);
    v.robot = mn_opt_bool(opts, "robot", v.robot, pos, w);
    const char *d = mn_opt_str(opts, "desc", NULL, pos, w);
    if (d) snprintf(v.desc, sizeof v.desc, "%s", d);
    if (v.f0 < 40 || v.f0 > 800) kc_error_at(pos, "tts.define_voice: f0 must be 40..800 Hz");
    if (v.tract < 0.6 || v.tract > 2.0) kc_error_at(pos, "tts.define_voice: tract must be 0.6..2.0");
    if (v.oq < 0.3 || v.oq > 0.9) kc_error_at(pos, "tts.define_voice: oq must be 0.3..0.9");
    if (v.rate < 0.3 || v.rate > 3.0) kc_error_at(pos, "tts.define_voice: rate must be 0.3..3.0");
    int idx = tts_voice_find(name);
    if (idx < 0) {                       /* allocate only after every value validated */
        if (NVOICES >= 32) kc_error_at(pos, "tts.define_voice: too many voices (limit 32)");
        idx = NVOICES++;
    }
    VOICES[idx] = v;
    return idx;
}
