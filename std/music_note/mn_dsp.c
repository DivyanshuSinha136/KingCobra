/* KingCobra std.music_note — DSP: filters, EQ, delay, reverb, modulation, dynamics, distortion,
 * convolution, time-stretch / pitch-shift / resample, and audio editing (slice, concat, mix, fade, ...).
 *
 * Every effect is non-destructive: it returns a NEW audio buffer and leaves its input untouched.
 * Every effect is also available through music_note.chain(audio, [{type: "...", ...}, ...]).
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mn_internal.h"

#define SETK(o, k, v) kc_object_set((o), (k), strlen(k), (v))

/* ============================================================ biquad */

void mn_biquad_set(MnBiquad *q, MnBqType t, double f, double Q, double gdb, double rate) {
    if (f < 1) f = 1;
    if (f > rate * 0.49) f = rate * 0.49;
    if (Q < 0.01) Q = 0.01;
    double w0 = MN_TAU * f / rate, cw = cos(w0), sw = sin(w0), al = sw / (2.0 * Q), A = pow(10.0, gdb / 40.0);
    double b0, b1, b2, a0, a1, a2, sq;
    switch (t) {
        case BQ_LOW:  b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case BQ_HIGH: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case BQ_BAND: b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case BQ_BANDQ: b0 = sw / 2; b1 = 0; b2 = -sw / 2; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case BQ_NOTCH: b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case BQ_ALLPASS: b0 = 1 - al; b1 = -2 * cw; b2 = 1 + al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case BQ_PEAK: b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; break;
        case BQ_LSHELF:
            sq = 2 * sqrt(A) * al;
            b0 = A * ((A + 1) - (A - 1) * cw + sq); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sq);
            a0 = (A + 1) + (A - 1) * cw + sq; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sq; break;
        default: /* BQ_HSHELF */
            sq = 2 * sqrt(A) * al;
            b0 = A * ((A + 1) + (A - 1) * cw + sq); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sq);
            a0 = (A + 1) - (A - 1) * cw + sq; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sq; break;
    }
    q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0; q->a1 = a1 / a0; q->a2 = a2 / a0;
}
int mn_bq_type_from_name(const char *s) {
    if (!strcasecmp(s, "lowpass") || !strcasecmp(s, "low") || !strcasecmp(s, "lp")) return BQ_LOW;
    if (!strcasecmp(s, "highpass") || !strcasecmp(s, "high") || !strcasecmp(s, "hp")) return BQ_HIGH;
    if (!strcasecmp(s, "bandpass") || !strcasecmp(s, "band") || !strcasecmp(s, "bp")) return BQ_BAND;
    if (!strcasecmp(s, "notch") || !strcasecmp(s, "bandstop")) return BQ_NOTCH;
    if (!strcasecmp(s, "allpass")) return BQ_ALLPASS;
    if (!strcasecmp(s, "peak") || !strcasecmp(s, "peaking") || !strcasecmp(s, "bell")) return BQ_PEAK;
    if (!strcasecmp(s, "lowshelf")) return BQ_LSHELF;
    if (!strcasecmp(s, "highshelf")) return BQ_HSHELF;
    return -1;
}

void mn_filter(MnAudio *a, MnBqType t, double freq, double Q, double gain_db) {
    for (int c = 0; c < a->ch; c++) {
        MnBiquad q; memset(&q, 0, sizeof q);
        mn_biquad_set(&q, t, freq, Q, gain_db, a->rate);
        for (size_t i = 0; i < a->frames; i++) a->d[i * a->ch + c] = (float)mn_biquad_proc(&q, a->d[i * a->ch + c]);
    }
}

/* ============================================================ FFT */

void mn_fft(double *re, double *im, size_t n, bool inverse) {
    static double *tc = NULL, *ts = NULL; static size_t tn = 0;
    if (tn != n) {
        free(tc); free(ts); tc = kc_malloc(n / 2 * sizeof(double) + 8); ts = kc_malloc(n / 2 * sizeof(double) + 8); tn = n;
        for (size_t i = 0; i < n / 2; i++) { tc[i] = cos(MN_TAU * (double)i / (double)n); ts[i] = sin(MN_TAU * (double)i / (double)n); }
    }
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        size_t half = len / 2, step = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < half; k++) {
                double wr = tc[k * step], wi = inverse ? ts[k * step] : -ts[k * step];
                size_t u = i + k, v = i + k + half;
                double xr = re[v] * wr - im[v] * wi, xi = re[v] * wi + im[v] * wr;
                re[v] = re[u] - xr; im[v] = im[u] - xi; re[u] += xr; im[u] += xi;
            }
        }
    }
    if (inverse) for (size_t i = 0; i < n; i++) { re[i] /= (double)n; im[i] /= (double)n; }
}

/* ============================================================ delay */

void mn_delay(MnAudio *a, double time_s, double fb, double mix, bool pingpong, double tail_s, KcPos pos, const char *who) {
    size_t d = (size_t)(time_s * a->rate); if (d < 1) d = 1;
    if (tail_s < 0) {
        double n = fb > 0.001 ? ceil(log(0.001) / log(fb)) : 1.0;
        tail_s = n * time_s; if (tail_s > 10) tail_s = 10;
    }
    size_t extra = (size_t)(tail_s * a->rate);
    size_t n0 = a->frames;
    if (extra) mn_audio_resize(a, n0 + extra, pos, who);
    int ch = a->ch;
    float *bufs = kc_malloc(d * (size_t)ch * sizeof(float)); memset(bufs, 0, d * (size_t)ch * sizeof(float));
    size_t idx = 0;
    bool pp = pingpong && ch == 2;
    for (size_t i = 0; i < a->frames; i++) {
        float *x = a->d + i * ch, *b = bufs + idx * ch;
        if (pp) {
            float dl = b[0], dr = b[1], xl = x[0], xr = x[1];
            b[0] = xl + (float)fb * dr; b[1] = xr + (float)fb * dl;
            x[0] = xl + (float)mix * dl; x[1] = xr + (float)mix * dr;
        } else for (int c = 0; c < ch; c++) {
            float dl = b[c], xin = x[c];
            b[c] = xin + (float)fb * dl; x[c] = xin + (float)mix * dl;
        }
        if (++idx >= d) idx = 0;
    }
    free(bufs);
}

/* ============================================================ Freeverb */

static void reverb_core(MnAudio *a, double room, double damp, double dry, double wet, double width, double predelay_s, double tail_s, KcPos pos, const char *who) {
    if (a->ch > 2) kc_error_at(pos, "music_note.%s: reverb supports mono or stereo audio", who);
    if (a->ch == 1) {        /* mono -> stereo so the reverb can spread */
        float *nd = kc_malloc(a->frames * 2 * sizeof(float));
        for (size_t i = 0; i < a->frames; i++) nd[2 * i] = nd[2 * i + 1] = a->d[i];
        free(a->d); a->d = nd; a->ch = 2;
    }
    if (room < 0) room = 0;
    if (room > 1) room = 1;
    if (damp < 0) damp = 0;
    if (damp > 1) damp = 1;
    if (tail_s < 0) tail_s = 0.5 + room * 3.5;
    size_t n0 = a->frames, extra = (size_t)(tail_s * a->rate);
    if (extra) mn_audio_resize(a, n0 + extra, pos, who);
    static const int CT[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 }, AT[4] = { 556, 441, 341, 225 };
    double sc = a->rate / 44100.0;
    float *cb[2][8]; int cs[2][8], ci[2][8]; double cst[2][8];
    float *ab[2][4]; int as_[2][4], ai[2][4];
    for (int c = 0; c < 2; c++) {
        for (int k = 0; k < 8; k++) { cs[c][k] = (int)((CT[k] + 23 * c) * sc) + 1; cb[c][k] = kc_malloc((size_t)cs[c][k] * sizeof(float)); memset(cb[c][k], 0, (size_t)cs[c][k] * sizeof(float)); ci[c][k] = 0; cst[c][k] = 0; }
        for (int k = 0; k < 4; k++) { as_[c][k] = (int)((AT[k] + 23 * c) * sc) + 1; ab[c][k] = kc_malloc((size_t)as_[c][k] * sizeof(float)); memset(ab[c][k], 0, (size_t)as_[c][k] * sizeof(float)); ai[c][k] = 0; }
    }
    size_t pd = (size_t)(predelay_s * a->rate); float *pbuf = NULL; size_t pi = 0;
    if (pd > 0) { pbuf = kc_malloc(pd * 2 * sizeof(float)); memset(pbuf, 0, pd * 2 * sizeof(float)); }
    double fbk = room * 0.28 + 0.7, d1 = damp * 0.4, d2 = 1.0 - d1;
    double w3 = wet * 3.0, wet1 = w3 * (width / 2.0 + 0.5), wet2 = w3 * ((1.0 - width) / 2.0);
    for (size_t i = 0; i < a->frames; i++) {
        float *x = a->d + i * 2;
        double xl = x[0], xr = x[1];
        double in_l = xl, in_r = xr;
        if (pbuf) { double ol = pbuf[pi * 2], orr = pbuf[pi * 2 + 1]; pbuf[pi * 2] = (float)xl; pbuf[pi * 2 + 1] = (float)xr; if (++pi >= pd) pi = 0; in_l = ol; in_r = orr; }
        double input = (in_l + in_r) * 0.015, out[2] = { 0, 0 };
        for (int c = 0; c < 2; c++) {
            double sum = 0;
            for (int k = 0; k < 8; k++) {
                float o = cb[c][k][ci[c][k]];
                cst[c][k] = o * d2 + cst[c][k] * d1;
                cb[c][k][ci[c][k]] = (float)(input + cst[c][k] * fbk);
                if (++ci[c][k] >= cs[c][k]) ci[c][k] = 0;
                sum += o;
            }
            for (int k = 0; k < 4; k++) {
                float bo = ab[c][k][ai[c][k]];
                double o = -sum + bo;
                ab[c][k][ai[c][k]] = (float)(sum + bo * 0.5);
                if (++ai[c][k] >= as_[c][k]) ai[c][k] = 0;
                sum = o;
            }
            out[c] = sum;
        }
        x[0] = (float)(xl * dry + out[0] * wet1 + out[1] * wet2);
        x[1] = (float)(xr * dry + out[1] * wet1 + out[0] * wet2);
    }
    for (int c = 0; c < 2; c++) { for (int k = 0; k < 8; k++) free(cb[c][k]); for (int k = 0; k < 4; k++) free(ab[c][k]); }
    free(pbuf);
}
void mn_reverb(MnAudio *a, double room, double damp, double wet, double width, double predelay_s, double tail_s, KcPos pos, const char *who) {
    reverb_core(a, room, damp, 1.0, wet, width, predelay_s, tail_s, pos, who);
}

/* ============================================================ modulated delay (chorus / flanger / vibrato) */

static void moddelay(MnAudio *a, double base_ms, double depth_ms, double rate_hz, double fb, double dry, double wet, int voices) {
    int ch = a->ch; double sr = a->rate;
    size_t maxd = (size_t)((base_ms + depth_ms + 2.0) * sr / 1000.0) + 4;
    float *buf = kc_malloc(maxd * (size_t)ch * sizeof(float)); memset(buf, 0, maxd * (size_t)ch * sizeof(float));
    size_t w = 0;
    for (size_t i = 0; i < a->frames; i++) {
        double t = (double)i / sr;
        for (int c = 0; c < ch; c++) {
            double acc = 0;
            for (int v = 0; v < voices; v++) {
                double ph = (double)v / voices + (c & 1 ? 0.25 : 0.0);
                double dms = base_ms + depth_ms * (0.5 + 0.5 * sin(MN_TAU * (rate_hz * t + ph)));
                double dd = dms * sr / 1000.0, rp = (double)w - dd; while (rp < 0) rp += (double)maxd;
                size_t i0 = (size_t)rp % maxd, i1 = (i0 + 1) % maxd; double fr = rp - floor(rp);
                acc += buf[i0 * ch + c] * (1.0 - fr) + buf[i1 * ch + c] * fr;
            }
            acc /= voices;
            float xin = a->d[i * ch + c];
            buf[w * ch + c] = xin + (float)(fb * acc);
            a->d[i * ch + c] = (float)(xin * dry + acc * wet);
        }
        if (++w >= maxd) w = 0;
    }
    free(buf);
}
void mn_chorus(MnAudio *a, double rate, double depth_ms, double mix, int voices) {
    moddelay(a, 12.0, depth_ms, rate, 0.0, 1.0 - 0.5 * mix, mix, voices < 1 ? 1 : voices);
}

/* ============================================================ limiter */

void mn_limit(MnAudio *a, double ceil_lin, double look_s, double rel_s) {
    size_t n = a->frames; int ch = a->ch; if (!n) return;
    size_t L = (size_t)(look_s * a->rate); if (L < 1) L = 1;
    float *target = kc_malloc((n + L) * sizeof(float));
    for (size_t i = 0; i < n; i++) {
        float pk = 0; for (int c = 0; c < ch; c++) { float v = fabsf(a->d[i * ch + c]); if (v > pk) pk = v; }
        target[i] = pk > ceil_lin ? (float)(ceil_lin / pk) : 1.0f;
    }
    for (size_t i = n; i < n + L; i++) target[i] = 1.0f;
    /* sliding window minimum over [i, i+L-1] */
    size_t *dq = kc_malloc((n + L) * sizeof(size_t)); size_t head = 0, tail = 0;
    float *gmin = kc_malloc(n * sizeof(float));
    for (size_t j = 0; j < n + L; j++) {
        while (tail > head && target[dq[tail - 1]] >= target[j]) tail--;
        dq[tail++] = j;
        if (j + 1 >= L) {
            size_t i = j + 1 - L;
            while (dq[head] < i) head++;
            if (i < n) gmin[i] = target[dq[head]];
        }
    }
    double run = 0; for (size_t k = 0; k < L; k++) run += gmin[0];
    double relc = rel_s > 0 ? 1.0 - exp(-1.0 / (rel_s * a->rate)) : 1.0, prev = 1.0;
    for (size_t p = 0; p < n; p++) {
        if (p > 0) { run += gmin[p]; run -= (p >= L) ? gmin[p - L] : gmin[0]; }
        else run = run - gmin[0] + gmin[0];
        double g = run / (double)L;
        double rg = prev + (1.0 - prev) * relc;
        if (g < rg) rg = g;
        prev = rg;
        for (int c = 0; c < ch; c++) {
            float v = (float)(a->d[p * ch + c] * rg);
            if (v > ceil_lin) v = (float)ceil_lin; else if (v < -ceil_lin) v = (float)-ceil_lin;
            a->d[p * ch + c] = v;
        }
    }
    free(target); free(dq); free(gmin);
}

/* ============================================================ effect plumbing */

typedef MnAudio *(*FxFn)(const MnAudio *a, KcValue o, KcPos pos, const char *who);

static double need_num(KcValue o, const char *key, KcPos pos, const char *who) {
    KcValue *v = mn_opt_get(o, key);
    if (!v || v->type == VAL_NULL) kc_error_at(pos, "music_note.%s: missing parameter '%s'", who, key);
    return mn_num(*v, pos, who, key);
}
static MnAudio *clone(const MnAudio *a, KcPos pos, const char *who) { return mn_audio_clone(a, pos, who); }
static MnAudio *same_shape(const MnAudio *a, size_t frames, int ch, KcPos pos, const char *who) { return mn_audio_new(frames, ch, a->rate, pos, who); }

static MnAudio *fx_filter(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    const char *tn = mn_opt_str(o, "type", "lowpass", pos, who);
    if (!strcasecmp(tn, "filter")) tn = "lowpass";
    int t = mn_bq_type_from_name(tn);
    if (t < 0) kc_error_at(pos, "music_note.%s: unknown filter type '%s' (lowpass highpass bandpass notch allpass peak lowshelf highshelf)", who, tn);
    double f = need_num(o, "freq", pos, who), q = mn_opt_num(o, "q", 0.7071, pos, who), g = mn_opt_num(o, "gain", 0.0, pos, who);
    int slope = (int)mn_opt_int(o, "slope", 12, pos, who);
    if (f <= 0 || f >= a->rate / 2.0) kc_error_at(pos, "music_note.%s: freq must be between 0 and half the sample rate (%d Hz)", who, a->rate / 2);
    if (q <= 0) kc_error_at(pos, "music_note.%s: q must be > 0", who);
    MnAudio *r = clone(a, pos, who);
    if ((t == BQ_LOW || t == BQ_HIGH) && slope > 12) {
        static const double Q24[2] = { 0.5412, 1.3066 }, Q48[4] = { 0.5098, 0.6013, 0.8999, 2.5629 };
        if (slope != 24 && slope != 48) kc_error_at(pos, "music_note.%s: slope must be 12, 24 or 48 (dB/octave)", who);
        int ns = slope == 24 ? 2 : 4; const double *qs = slope == 24 ? Q24 : Q48;
        for (int k = 0; k < ns; k++) mn_filter(r, (MnBqType)t, f, qs[k], 0);
    } else mn_filter(r, (MnBqType)t, f, q, g);
    return r;
}
/* eq: bands = [{type, freq, gain, q}] */
static MnAudio *fx_eq(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    KcValue *b = mn_opt_get(o, "bands");
    if (!b || b->type != VAL_ARRAY) kc_error_at(pos, "music_note.%s: 'bands' must be an array of strings like \"peak freq=1000 gain=3 q=1\"", who);
    MnAudio *r = clone(a, pos, who);
    for (int i = 0; i < b->as.arr->count; i++) {
        KcValue bo = b->as.arr->items[i];
        if (bo.type != VAL_STRING && !mn_is_opts(bo)) kc_error_at(pos, "music_note.%s: each band must be a string like \"peak freq=1000 gain=3 q=1\"", who);
        const char *tn = mn_opt_str(bo, "type", "peak", pos, who); int t = mn_bq_type_from_name(tn);
        if (t < 0) kc_error_at(pos, "music_note.%s: unknown band type '%s'", who, tn);
        double f = need_num(bo, "freq", pos, who);
        if (f <= 0 || f >= a->rate / 2.0) kc_error_at(pos, "music_note.%s: band freq out of range", who);
        mn_filter(r, (MnBqType)t, f, mn_opt_num(bo, "q", 1.0, pos, who), mn_opt_num(bo, "gain", 0.0, pos, who));
    }
    return r;
}
static MnAudio *fx_delay(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    double t = need_num(o, "time", pos, who), fb = mn_opt_num(o, "feedback", 0.4, pos, who), mix = mn_opt_num(o, "mix", 0.35, pos, who);
    if (t <= 0 || t > 10) kc_error_at(pos, "music_note.%s: time must be 0..10 seconds", who);
    if (fb < 0 || fb > 0.98) kc_error_at(pos, "music_note.%s: feedback must be 0..0.98", who);
    MnAudio *r = clone(a, pos, who);
    mn_delay(r, t, fb, mix, mn_opt_bool(o, "pingpong", false, pos, who), mn_opt_num(o, "tail", -1, pos, who), pos, who);
    return r;
}
static MnAudio *fx_reverb(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    double mix = mn_opt_num(o, "mix", 0.3, pos, who);
    MnAudio *r = clone(a, pos, who);
    reverb_core(r, mn_opt_num(o, "room", 0.7, pos, who), mn_opt_num(o, "damp", 0.5, pos, who), 1.0 - mix, mix, mn_opt_num(o, "width", 1.0, pos, who),
                mn_opt_num(o, "predelay", 0.0, pos, who), mn_opt_num(o, "tail", -1, pos, who), pos, who);
    return r;
}
static MnAudio *fx_chorus(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    mn_chorus(r, mn_opt_num(o, "rate", 0.8, pos, who), mn_opt_num(o, "depth", 6.0, pos, who), mn_opt_num(o, "mix", 0.5, pos, who), (int)mn_opt_int(o, "voices", 3, pos, who));
    return r;
}
static MnAudio *fx_flanger(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double fb = mn_opt_num(o, "feedback", 0.5, pos, who), mix = mn_opt_num(o, "mix", 0.5, pos, who), dep = mn_opt_num(o, "depth", 2.0, pos, who);
    if (fb < -0.95 || fb > 0.95) kc_error_at(pos, "music_note.%s: feedback must be -0.95..0.95", who);
    moddelay(r, 0.5, dep, mn_opt_num(o, "rate", 0.25, pos, who), fb, 1.0 - 0.5 * mix, mix, 1);
    return r;
}
static MnAudio *fx_vibrato(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double rate = mn_opt_num(o, "rate", 5.0, pos, who), cents = mn_opt_num(o, "depth", 30.0, pos, who);
    double dms = cents / 1200.0 * 1000.0 / (MN_TAU * rate) * MN_TAU * 0.5;   /* peak delay swing producing +-cents of pitch */
    if (dms < 0.01) dms = 0.01;
    moddelay(r, dms + 0.5, dms * 2, rate, 0.0, 0.0, 1.0, 1);
    return r;
}
static MnAudio *fx_phaser(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double rate = mn_opt_num(o, "rate", 0.4, pos, who), depth = mn_opt_num(o, "depth", 0.8, pos, who), fb = mn_opt_num(o, "feedback", 0.4, pos, who), mix = mn_opt_num(o, "mix", 0.5, pos, who);
    int st = (int)mn_opt_int(o, "stages", 6, pos, who); if (st < 2) st = 2; if (st > 12) st = 12;
    double fmin = mn_opt_num(o, "min_freq", 200, pos, who), fmax = mn_opt_num(o, "max_freq", 2400, pos, who);
    for (int c = 0; c < r->ch; c++) {
        double z[12] = { 0 }, last = 0;
        for (size_t i = 0; i < r->frames; i++) {
            double t = (double)i / r->rate, l = 0.5 + 0.5 * sin(MN_TAU * (rate * t + (c & 1 ? 0.25 : 0)));
            double f = fmin * pow(fmax / fmin, l * depth);
            double tn = tan(M_PI * f / r->rate), ac = (tn - 1.0) / (tn + 1.0);
            double x = r->d[i * r->ch + c], y = x + fb * last;
            for (int s = 0; s < st; s++) { double o2 = ac * y + z[s]; z[s] = y - ac * o2; y = o2; }
            last = y;
            r->d[i * r->ch + c] = (float)(x * (1.0 - mix) + y * mix);
        }
    }
    return r;
}
static MnAudio *fx_tremolo(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double rate = mn_opt_num(o, "rate", 5.0, pos, who), depth = mn_opt_num(o, "depth", 0.5, pos, who);
    const char *sh = mn_opt_str(o, "shape", "sine", pos, who);
    int shape = !strcasecmp(sh, "triangle") ? 1 : !strcasecmp(sh, "square") ? 2 : 0;
    if (depth < 0) depth = 0;
    if (depth > 1) depth = 1;
    for (size_t i = 0; i < r->frames; i++) {
        double ph = fmod(rate * (double)i / r->rate, 1.0), l;
        l = shape == 0 ? 0.5 + 0.5 * sin(MN_TAU * ph) : shape == 1 ? 1.0 - fabs(2.0 * ph - 1.0) : (ph < 0.5 ? 1.0 : 0.0);
        float g = (float)(1.0 - depth + depth * l);
        for (int c = 0; c < r->ch; c++) r->d[i * r->ch + c] *= g;
    }
    return r;
}
static MnAudio *fx_ringmod(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double f = need_num(o, "freq", pos, who), mix = mn_opt_num(o, "mix", 1.0, pos, who);
    for (size_t i = 0; i < r->frames; i++) {
        double m = sin(MN_TAU * f * (double)i / r->rate);
        for (int c = 0; c < r->ch; c++) { float x = r->d[i * r->ch + c]; r->d[i * r->ch + c] = (float)(x * (1.0 - mix) + x * m * mix); }
    }
    return r;
}
static MnAudio *fx_distortion(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double drive = mn_opt_num(o, "drive", 4.0, pos, who), mix = mn_opt_num(o, "mix", 1.0, pos, who), level = mn_opt_num(o, "level", 0.7, pos, who), tone = mn_opt_num(o, "tone", 0.0, pos, who);
    const char *kind = mn_opt_str(o, "kind", "soft", pos, who);
    int k = !strcasecmp(kind, "soft") ? 0 : !strcasecmp(kind, "hard") ? 1 : !strcasecmp(kind, "fold") ? 2 : !strcasecmp(kind, "tube") ? 3 : -1;
    if (k < 0) kc_error_at(pos, "music_note.%s: unknown kind '%s' (soft hard fold tube)", who, kind);
    if (drive < 0.01) drive = 0.01;
    double norm = tanh(drive);
    for (size_t i = 0; i < r->frames * r->ch; i++) {
        double x = r->d[i], y;
        switch (k) {
            case 0: y = tanh(drive * x) / norm; break;
            case 1: y = fmax(-1.0, fmin(1.0, drive * x)); break;
            case 2: y = (2.0 / M_PI) * asin(sin(drive * x * M_PI / 2.0)); break;
            default: y = x >= 0 ? tanh(drive * x) / norm : tanh(0.55 * drive * x) / tanh(0.55 * drive); break;
        }
        r->d[i] = (float)((x * (1.0 - mix) + y * mix) * level);
    }
    if (k == 3) mn_filter(r, BQ_HIGH, 10.0, 0.7071, 0);
    if (tone > 0 && tone < r->rate / 2.0) mn_filter(r, BQ_LOW, tone, 0.7071, 0);
    return r;
}
static MnAudio *fx_bitcrush(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    int bits = (int)mn_opt_int(o, "bits", 8, pos, who), ds = (int)mn_opt_int(o, "downsample", 1, pos, who);
    double mix = mn_opt_num(o, "mix", 1.0, pos, who);
    if (bits < 1 || bits > 24) kc_error_at(pos, "music_note.%s: bits must be 1..24", who);
    if (ds < 1 || ds > 512) kc_error_at(pos, "music_note.%s: downsample must be 1..512", who);
    double lv = pow(2.0, bits - 1);
    for (int c = 0; c < r->ch; c++) {
        float hold = 0;
        for (size_t i = 0; i < r->frames; i++) {
            float x = r->d[i * r->ch + c];
            if (i % (size_t)ds == 0) hold = (float)(round(x * lv) / lv);
            r->d[i * r->ch + c] = (float)(x * (1.0 - mix) + hold * mix);
        }
    }
    return r;
}
static MnAudio *fx_compressor(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double thr = mn_opt_num(o, "threshold", -18.0, pos, who), ratio = mn_opt_num(o, "ratio", 4.0, pos, who);
    double att = mn_opt_num(o, "attack", 0.01, pos, who), rel = mn_opt_num(o, "release", 0.12, pos, who), knee = mn_opt_num(o, "knee", 6.0, pos, who);
    double makeup;
    KcValue *mv = mn_opt_get(o, "makeup");
    if (mv && mv->type == VAL_STRING && !strcasecmp(mv->as.s->data, "auto")) makeup = -thr * (1.0 - 1.0 / ratio) * 0.5;
    else makeup = mn_opt_num(o, "makeup", 0.0, pos, who);
    if (ratio < 1) kc_error_at(pos, "music_note.%s: ratio must be >= 1", who);
    double ca = exp(-1.0 / (fmax(att, 1e-4) * r->rate)), cr = exp(-1.0 / (fmax(rel, 1e-3) * r->rate)), env = 0;
    double mk = mn_db2lin(makeup);
    for (size_t i = 0; i < r->frames; i++) {
        double pk = 0; for (int c = 0; c < r->ch; c++) { double v = fabs(r->d[i * r->ch + c]); if (v > pk) pk = v; }
        env = pk > env ? ca * env + (1.0 - ca) * pk : cr * env + (1.0 - cr) * pk;
        double over = mn_lin2db(env) - thr, gr = 0;
        if (2.0 * over < -knee) gr = 0;
        else if (knee > 0 && 2.0 * fabs(over) <= knee) { double x = over + knee / 2.0; gr = (1.0 / ratio - 1.0) * x * x / (2.0 * knee); }
        else gr = (1.0 / ratio - 1.0) * over;
        float g = (float)(mn_db2lin(gr) * mk);
        for (int c = 0; c < r->ch; c++) r->d[i * r->ch + c] *= g;
    }
    return r;
}
static MnAudio *fx_limiter(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    mn_limit(r, mn_db2lin(mn_opt_num(o, "ceiling", -1.0, pos, who)), mn_opt_num(o, "lookahead", 0.005, pos, who), mn_opt_num(o, "release", 0.1, pos, who));
    return r;
}
static MnAudio *fx_gate(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double thr = mn_db2lin(mn_opt_num(o, "threshold", -50.0, pos, who)), floor_g = mn_db2lin(mn_opt_num(o, "range", -80.0, pos, who));
    double att = mn_opt_num(o, "attack", 0.002, pos, who), rel = mn_opt_num(o, "release", 0.1, pos, who), hold = mn_opt_num(o, "hold", 0.02, pos, who);
    double ca = exp(-1.0 / (fmax(att, 1e-4) * r->rate)), cr = exp(-1.0 / (fmax(rel, 1e-3) * r->rate)), g = floor_g, env = 0;
    size_t hold_n = (size_t)(hold * r->rate), held = 0;
    for (size_t i = 0; i < r->frames; i++) {
        double pk = 0; for (int c = 0; c < r->ch; c++) { double v = fabs(r->d[i * r->ch + c]); if (v > pk) pk = v; }
        env = pk > env ? pk : env * 0.9995 + pk * 0.0005;
        double target;
        if (env > thr) { target = 1.0; held = hold_n; } else if (held > 0) { held--; target = 1.0; } else target = floor_g;
        g = target > g ? ca * g + (1.0 - ca) * target : cr * g + (1.0 - cr) * target;
        for (int c = 0; c < r->ch; c++) r->d[i * r->ch + c] *= (float)g;
    }
    return r;
}
static MnAudio *fx_dc_block(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double R = 1.0 - (MN_TAU * 8.0 / r->rate);
    for (int c = 0; c < r->ch; c++) {
        double xp = 0, yp = 0;
        for (size_t i = 0; i < r->frames; i++) { double x = r->d[i * r->ch + c], y = x - xp + R * yp; xp = x; yp = y; r->d[i * r->ch + c] = (float)y; }
    }
    return r;
}
static MnAudio *fx_gain(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    KcValue *f = mn_opt_get(o, "factor");
    float g = f ? (float)mn_num(*f, pos, who, "factor") : (float)mn_db2lin(need_num(o, "db", pos, who));
    for (size_t i = 0; i < r->frames * r->ch; i++) r->d[i] *= g;
    return r;
}
static MnAudio *fx_normalize(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    float pk = 0; for (size_t i = 0; i < r->frames * r->ch; i++) { float v = fabsf(r->d[i]); if (v > pk) pk = v; }
    if (pk > 1e-9f) { float g = (float)(mn_db2lin(mn_opt_num(o, "peak", -1.0, pos, who)) / pk); for (size_t i = 0; i < r->frames * r->ch; i++) r->d[i] *= g; }
    return r;
}
static MnAudio *fx_fade(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double fin = mn_opt_num(o, "in", 0.0, pos, who), fout = mn_opt_num(o, "out", 0.0, pos, who);
    const char *cv = mn_opt_str(o, "curve", "linear", pos, who);
    int cu = !strcasecmp(cv, "exp") ? 1 : !strcasecmp(cv, "cos") ? 2 : 0;
    size_t ni = (size_t)(fin * r->rate), no = (size_t)(fout * r->rate);
    if (ni > r->frames) ni = r->frames;
    if (no > r->frames) no = r->frames;
    for (size_t i = 0; i < ni; i++) {
        double u = (double)i / (double)ni, g = cu == 1 ? u * u : cu == 2 ? 0.5 - 0.5 * cos(M_PI * u) : u;
        for (int c = 0; c < r->ch; c++) r->d[i * r->ch + c] *= (float)g;
    }
    for (size_t i = 0; i < no; i++) {
        double u = (double)i / (double)no, g = cu == 1 ? u * u : cu == 2 ? 0.5 - 0.5 * cos(M_PI * u) : u;
        size_t p = r->frames - 1 - i;
        for (int c = 0; c < r->ch; c++) r->d[p * r->ch + c] *= (float)g;
    }
    return r;
}
static MnAudio *fx_reverse(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = same_shape(a, a->frames, a->ch, pos, who);
    for (size_t i = 0; i < a->frames; i++) memcpy(r->d + i * a->ch, a->d + (a->frames - 1 - i) * a->ch, (size_t)a->ch * sizeof(float));
    return r;
}
static MnAudio *fx_pan(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    double p = need_num(o, "pos", pos, who);
    if (p < -1 || p > 1) kc_error_at(pos, "music_note.%s: pan position must be -1 (left) .. 1 (right)", who);
    double ang = (p + 1.0) * M_PI / 4.0; float gl = (float)cos(ang), gr = (float)sin(ang);
    if (a->ch > 2) kc_error_at(pos, "music_note.%s: pan needs mono or stereo audio", who);
    MnAudio *r = same_shape(a, a->frames, 2, pos, who);
    for (size_t i = 0; i < a->frames; i++) {
        float l = a->d[i * a->ch], rr = a->ch == 2 ? a->d[i * 2 + 1] : l;
        if (a->ch == 1) { r->d[2 * i] = l * gl * 1.4142f; r->d[2 * i + 1] = l * gr * 1.4142f; }
        else { r->d[2 * i] = l * gl * 1.4142f; r->d[2 * i + 1] = rr * gr * 1.4142f; }
    }
    return r;
}
static MnAudio *fx_width(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    if (a->ch != 2) kc_error_at(pos, "music_note.%s: width needs stereo audio", who);
    double w = need_num(o, "amount", pos, who);
    MnAudio *r = clone(a, pos, who);
    for (size_t i = 0; i < r->frames; i++) {
        float l = r->d[2 * i], rr = r->d[2 * i + 1], m = 0.5f * (l + rr), s = 0.5f * (l - rr) * (float)w;
        r->d[2 * i] = m + s; r->d[2 * i + 1] = m - s;
    }
    return r;
}
static MnAudio *fx_to_mono(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = same_shape(a, a->frames, 1, pos, who);
    for (size_t i = 0; i < a->frames; i++) { float s = 0; for (int c = 0; c < a->ch; c++) s += a->d[i * a->ch + c]; r->d[i] = s / (float)a->ch; }
    return r;
}
static MnAudio *fx_to_stereo(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    if (a->ch == 2) return clone(a, pos, who);
    MnAudio *r = same_shape(a, a->frames, 2, pos, who);
    for (size_t i = 0; i < a->frames; i++) { r->d[2 * i] = a->d[i * a->ch]; r->d[2 * i + 1] = a->ch > 1 ? a->d[i * a->ch + 1] : a->d[i]; }
    return r;
}
static MnAudio *fx_swap(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    if (a->ch != 2) kc_error_at(pos, "music_note.%s: swap_channels needs stereo audio", who);
    MnAudio *r = clone(a, pos, who);
    for (size_t i = 0; i < r->frames; i++) { float t = r->d[2 * i]; r->d[2 * i] = r->d[2 * i + 1]; r->d[2 * i + 1] = t; }
    return r;
}
static MnAudio *fx_channel(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    int64_t c = (int64_t)need_num(o, "index", pos, who);
    if (c < 0 || c >= a->ch) kc_error_at(pos, "music_note.%s: channel %lld does not exist (audio has %d)", who, (long long)c, a->ch);
    MnAudio *r = same_shape(a, a->frames, 1, pos, who);
    for (size_t i = 0; i < a->frames; i++) r->d[i] = a->d[i * a->ch + c];
    return r;
}
static MnAudio *fx_slice(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    double s = mn_opt_num(o, "start", 0.0, pos, who), e = mn_opt_num(o, "end", -1.0, pos, who);
    size_t f0 = (size_t)(s < 0 ? 0 : s * a->rate), f1 = e < 0 ? a->frames : (size_t)(e * a->rate);
    if (f1 > a->frames) f1 = a->frames;
    if (f0 > f1) f0 = f1;
    MnAudio *r = same_shape(a, f1 - f0, a->ch, pos, who);
    memcpy(r->d, a->d + f0 * a->ch, (f1 - f0) * (size_t)a->ch * sizeof(float));
    return r;
}
static MnAudio *fx_loop(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    int64_t n = (int64_t)need_num(o, "times", pos, who);
    if (n < 1 || n > 100000) kc_error_at(pos, "music_note.%s: times must be 1..100000", who);
    MnAudio *r = same_shape(a, a->frames * (size_t)n, a->ch, pos, who);
    for (int64_t k = 0; k < n; k++) memcpy(r->d + (size_t)k * a->frames * a->ch, a->d, a->frames * (size_t)a->ch * sizeof(float));
    return r;
}
static MnAudio *fx_trim(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    double thr = mn_db2lin(mn_opt_num(o, "threshold", -60.0, pos, who)); size_t pad = (size_t)(mn_opt_num(o, "pad", 0.0, pos, who) * a->rate);
    size_t lo = 0, hi = a->frames;
    while (lo < hi) { bool loud = false; for (int c = 0; c < a->ch; c++) if (fabsf(a->d[lo * a->ch + c]) > thr) loud = true; if (loud) break; lo++; }
    while (hi > lo) { bool loud = false; for (int c = 0; c < a->ch; c++) if (fabsf(a->d[(hi - 1) * a->ch + c]) > thr) loud = true; if (loud) break; hi--; }
    lo = lo > pad ? lo - pad : 0; hi = hi + pad < a->frames ? hi + pad : a->frames;
    MnAudio *r = same_shape(a, hi - lo, a->ch, pos, who);
    memcpy(r->d, a->d + lo * a->ch, (hi - lo) * (size_t)a->ch * sizeof(float));
    return r;
}

/* ---- resampling (Hermite interpolation, anti-alias lowpass when shrinking) ---- */
static inline float hermite(float y0, float y1, float y2, float y3, double x) {
    double c0 = y1, c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return (float)(((c3 * x + c2) * x + c1) * x + c0);
}
/* read the source at a fractional step; out rate = out_rate */
static MnAudio *resample_core(const MnAudio *a, double step, int out_rate, KcPos pos, const char *who) {
    const MnAudio *src = a; MnAudio *tmp = NULL;
    if (step > 1.0) {      /* going to fewer samples per second of material: remove content above the new Nyquist first */
        tmp = clone(a, pos, who);
        double cut = a->rate * 0.45 / step;
        mn_filter(tmp, BQ_LOW, cut, 0.5412, 0); mn_filter(tmp, BQ_LOW, cut, 1.3066, 0);
        src = tmp;
    }
    size_t nf = (size_t)((double)a->frames / step);
    MnAudio *r = mn_audio_new(nf, a->ch, out_rate, pos, who);
    int ch = a->ch; long n = (long)src->frames;
    for (size_t i = 0; i < nf; i++) {
        double x = (double)i * step; long i1 = (long)x; double fr = x - (double)i1;
        for (int c = 0; c < ch; c++) {
            long i0 = i1 - 1, i2 = i1 + 1, i3 = i1 + 2;
            float y0 = i0 >= 0 ? src->d[i0 * ch + c] : 0, y1 = i1 < n ? src->d[i1 * ch + c] : 0;
            float y2 = i2 < n ? src->d[i2 * ch + c] : 0, y3 = i3 < n ? src->d[i3 * ch + c] : 0;
            r->d[i * ch + c] = hermite(y0, y1, y2, y3, fr);
        }
    }
    if (tmp) mn_audio_free(tmp);
    return r;
}
MnAudio *mn_resample_step(const MnAudio *a, double step, int out_rate, KcPos pos, const char *who) { return resample_core(a, step, out_rate, pos, who); }
static MnAudio *wsola(const MnAudio *a, double factor, KcPos pos, const char *who);
MnAudio *mn_time_stretch(const MnAudio *a, double factor, KcPos pos, const char *who) {
    if (a->frames < 4096 || fabs(factor - 1.0) < 0.002) return mn_audio_clone(a, pos, who);
    if (factor < 0.25) factor = 0.25;
    if (factor > 4.0) factor = 4.0;
    return wsola(a, factor, pos, who);
}
static MnAudio *fx_resample(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    int64_t rt = (int64_t)need_num(o, "rate", pos, who);
    if (rt < 1000 || rt > 384000) kc_error_at(pos, "music_note.%s: rate must be 1000..384000", who);
    if (rt == a->rate) return clone(a, pos, who);
    return resample_core(a, (double)a->rate / (double)rt, (int)rt, pos, who);
}
static MnAudio *fx_speed(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    double f = need_num(o, "factor", pos, who);
    if (f < 0.05 || f > 20) kc_error_at(pos, "music_note.%s: factor must be 0.05..20 (2 = twice as fast and an octave higher)", who);
    return resample_core(a, f, a->rate, pos, who);
}

/* ---- WSOLA time stretch (pitch preserved) ---- */
static MnAudio *wsola(const MnAudio *a, double factor, KcPos pos, const char *who) {
    int ch = a->ch;
    long N = 2048; long Hs = N / 2, Delta = 384;
    double Ha = (double)Hs / factor;
    size_t nout = (size_t)((double)a->frames * factor) + (size_t)N;
    MnAudio *r = mn_audio_new(nout, ch, a->rate, pos, who);
    float *norm = kc_malloc(nout * sizeof(float)); memset(norm, 0, nout * sizeof(float));
    /* mono, 4x-decimated copy used only to rate candidate offsets */
    long nin = (long)a->frames; long nd = nin / 4 + 1;
    float *m4 = kc_malloc((size_t)nd * sizeof(float));
    for (long i = 0; i < nd; i++) { float s = 0; long p = i * 4; if (p < nin) for (int c = 0; c < ch; c++) s += a->d[p * ch + c]; m4[i] = s; }
    float *win = kc_malloc((size_t)N * sizeof(float));
    for (long i = 0; i < N; i++) win[i] = (float)(0.5 - 0.5 * cos(MN_TAU * (double)i / (double)N));
    long prev = 0; bool first = true;
    for (size_t k = 0; ; k++) {
        long outpos = (long)(k * (size_t)Hs); if ((size_t)outpos >= a->frames * factor + 1) break;
        long nominal = (long)((double)k * Ha), pos_in = nominal;
        if (!first) {
            long target = prev + Hs, best = nominal, bestd = 0; double bestc = -1e30;
            for (long dlt = -Delta; dlt <= Delta; dlt += 4) {
                long cand = nominal + dlt; if (cand < 0 || cand + N > nin + N) continue;
                double cc = 0; long M = N / 4;
                for (long j = 0; j < M; j += 2) {
                    long u = target / 4 + j, v = cand / 4 + j;
                    if (u < nd && v < nd) cc += (double)m4[u] * m4[v];
                }
                if (cc > bestc) { bestc = cc; best = cand; bestd = dlt; }
            }
            (void)bestd; pos_in = best;
        }
        first = false; prev = pos_in;
        for (long i = 0; i < N; i++) {
            long si = pos_in + i, di = outpos + i;
            if ((size_t)di >= nout) break;
            float w = win[i]; norm[di] += w;
            if (si >= 0 && si < nin) for (int c = 0; c < ch; c++) r->d[di * ch + c] += a->d[si * ch + c] * w;
        }
        if (pos_in > nin + N) break;
    }
    size_t used = (size_t)((double)a->frames * factor);
    for (size_t i = 0; i < nout; i++) { float n = norm[i] > 1e-3f ? norm[i] : 1.0f; for (int c = 0; c < ch; c++) r->d[i * ch + c] /= n; }
    mn_audio_resize(r, used, pos, who);
    free(norm); free(m4); free(win);
    return r;
}
static MnAudio *fx_time_stretch(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    double f = need_num(o, "factor", pos, who);
    if (f < 0.25 || f > 4.0) kc_error_at(pos, "music_note.%s: factor must be 0.25..4 (2 = twice as long, same pitch)", who);
    if (a->frames < 4096) return clone(a, pos, who);
    return wsola(a, f, pos, who);
}
static MnAudio *fx_pitch_shift(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    double st = need_num(o, "semitones", pos, who);
    if (st < -24 || st > 24) kc_error_at(pos, "music_note.%s: semitones must be -24..24", who);
    if (st == 0) return clone(a, pos, who);
    double ratio = pow(2.0, st / 12.0);
    MnAudio *stretched = a->frames < 4096 ? clone(a, pos, who) : wsola(a, ratio, pos, who);
    MnAudio *r = resample_core(stretched, ratio, a->rate, pos, who);
    mn_audio_free(stretched);
    return r;
}

/* ---- convolution ---- */
static MnAudio *convolve_core(const MnAudio *a, const MnAudio *ir, double mix, bool norm, KcPos pos, const char *who) {
    if (ir->rate != a->rate) kc_error_at(pos, "music_note.%s: the impulse response is %d Hz but the audio is %d Hz (use resample first)", who, ir->rate, a->rate);
    size_t irn = ir->frames; if (!irn) kc_error_at(pos, "music_note.%s: the impulse response is empty", who);
    int och = a->ch == 1 && ir->ch > 1 ? ir->ch : a->ch;
    size_t N = 4096; while (N < irn * 2) N <<= 1;
    size_t B = N - irn + 1;
    size_t outn = a->frames + irn - 1;
    MnAudio *r = mn_audio_new(outn, och, a->rate, pos, who);
    double *xr = kc_malloc(N * sizeof(double)), *xi = kc_malloc(N * sizeof(double));
    double *hr = kc_malloc(N * sizeof(double)), *hi = kc_malloc(N * sizeof(double));
    for (int c = 0; c < och; c++) {
        int ic = ir->ch == 1 ? 0 : (c < ir->ch ? c : ir->ch - 1), ac = a->ch == 1 ? 0 : c;
        double energy = 0;
        for (size_t i = 0; i < irn; i++) { double v = ir->d[i * ir->ch + ic]; energy += v * v; }
        double sc = norm && energy > 1e-12 ? 1.0 / sqrt(energy) : 1.0;
        memset(hr, 0, N * sizeof(double)); memset(hi, 0, N * sizeof(double));
        for (size_t i = 0; i < irn; i++) hr[i] = ir->d[i * ir->ch + ic] * sc;
        mn_fft(hr, hi, N, false);
        for (size_t pos0 = 0; pos0 < a->frames; pos0 += B) {
            size_t cnt = a->frames - pos0 < B ? a->frames - pos0 : B;
            memset(xr, 0, N * sizeof(double)); memset(xi, 0, N * sizeof(double));
            for (size_t i = 0; i < cnt; i++) xr[i] = a->d[(pos0 + i) * a->ch + ac];
            mn_fft(xr, xi, N, false);
            for (size_t k = 0; k < N; k++) { double re = xr[k] * hr[k] - xi[k] * hi[k], im = xr[k] * hi[k] + xi[k] * hr[k]; xr[k] = re; xi[k] = im; }
            mn_fft(xr, xi, N, true);
            size_t lim = cnt + irn - 1;
            for (size_t i = 0; i < lim && pos0 + i < outn; i++) r->d[(pos0 + i) * och + c] += (float)xr[i];
        }
    }
    free(xr); free(xi); free(hr); free(hi);
    if (mix < 1.0) {
        for (size_t i = 0; i < outn; i++) for (int c = 0; c < och; c++) {
            float dry = i < a->frames ? a->d[i * a->ch + (a->ch == 1 ? 0 : c)] : 0.0f;
            r->d[i * och + c] = dry * (float)(1.0 - mix) + r->d[i * och + c] * (float)mix;
        }
    }
    return r;
}
static MnAudio *fx_convolve(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    KcValue *irv = mn_opt_get(o, "ir");
    if (!irv) kc_error_at(pos, "music_note.%s: missing parameter 'ir' (an impulse-response audio buffer)", who);
    const MnAudio *ir = mn_audio_arg(*irv, pos, who);
    return convolve_core(a, ir, mn_opt_num(o, "mix", 1.0, pos, who), mn_opt_bool(o, "normalize", true, pos, who), pos, who);
}


/* ---- production effects: tape saturation, vinyl, auto-wah, sidechain, mastering ---- */
static MnAudio *fx_saturate(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double drive = mn_opt_num(o, "drive", 2.0, pos, who), mix = mn_opt_num(o, "mix", 1.0, pos, who);
    double bias = mn_opt_num(o, "bias", 0.1, pos, who), tone = mn_opt_num(o, "tone", 0.0, pos, who);
    if (drive < 0.1 || drive > 30) kc_error_at(pos, "music_note.%s: drive must be 0.1..30", who);
    double tb = tanh(bias), norm = tanh(drive + bias) - tb; if (norm < 0.05) norm = 0.05;
    for (size_t i = 0; i < r->frames * r->ch; i++) {
        double x = r->d[i], y = (tanh(drive * x + bias) - tb) / norm;      /* asymmetric -> even harmonics, tape/tube warmth */
        r->d[i] = (float)(x * (1.0 - mix) + y * mix);
    }
    { MnAudio *d = fx_dc_block(r, o, pos, who); mn_audio_free(r); r = d; }
    if (tone > 0 && tone < r->rate / 2.0) mn_filter(r, BQ_LOW, tone, 0.7071, 0);
    return r;
}
static MnAudio *fx_vinyl(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double level = mn_opt_num(o, "level", 0.015, pos, who), crackle = mn_opt_num(o, "crackle", 1.0, pos, who);
    double hiss = mn_opt_num(o, "hiss", 0.5, pos, who), rumble = mn_opt_num(o, "rumble", 0.5, pos, who);
    for (int c = 0; c < r->ch; c++) {
        MnRng rng = { (uint64_t)mn_opt_int(o, "seed", 1, pos, who) * 7777u + (uint64_t)c * 131u + 9 };
        MnBiquad hp = {0}, lp = {0}; mn_biquad_set(&hp, BQ_HIGH, 3500, 0.7, 0, r->rate); mn_biquad_set(&lp, BQ_LOW, 45, 0.7, 0, r->rate);
        MnBiquad cb = {0}; mn_biquad_set(&cb, BQ_BAND, 2800, 0.8, 0, r->rate);
        double pop = 0, density = 6.0 * crackle / r->rate;
        for (size_t i = 0; i < r->frames; i++) {
            double n = mn_rng_f(&rng), y = mn_biquad_proc(&hp, n) * hiss * 0.35 + mn_biquad_proc(&lp, n) * rumble * 3.0;
            if ((mn_rng_f(&rng) + 1.0) * 0.5 < density) pop = mn_rng_f(&rng) * (0.4 + 1.6 * (mn_rng_f(&rng) + 1.0) * 0.5);
            y += mn_biquad_proc(&cb, pop) * 2.0; pop *= 0.55;
            r->d[i * r->ch + c] += (float)(y * level);
        }
    }
    return r;
}
static MnAudio *fx_wah(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    MnAudio *r = clone(a, pos, who);
    double rate = mn_opt_num(o, "rate", 1.5, pos, who), depth = mn_opt_num(o, "depth", 1.0, pos, who), q = mn_opt_num(o, "q", 4.0, pos, who);
    double f_lo = mn_opt_num(o, "min_freq", 400, pos, who), f_hi = mn_opt_num(o, "max_freq", 2400, pos, who), mix = mn_opt_num(o, "mix", 1.0, pos, who);
    double sens = mn_opt_num(o, "sensitivity", 6.0, pos, who);
    const char *mode = mn_opt_str(o, "mode", "lfo", pos, who);
    bool env_mode = !strcasecmp(mode, "env") || !strcasecmp(mode, "envelope");
    for (int c = 0; c < r->ch; c++) {
        MnBiquad bq; memset(&bq, 0, sizeof bq); double env = 0;
        double ca = exp(-1.0 / (0.004 * r->rate)), cr = exp(-1.0 / (0.09 * r->rate));
        for (size_t i = 0; i < r->frames; i++) {
            double x = r->d[i * r->ch + c], ax = fabs(x);
            env = ax > env ? ca * env + (1 - ca) * ax : cr * env + (1 - cr) * ax;
            if ((i & 15) == 0) {
                double l = env_mode ? fmin(1.0, env * sens) : 0.5 + 0.5 * sin(MN_TAU * (rate * (double)i / r->rate + (c & 1 ? 0.15 : 0)));
                mn_biquad_set(&bq, BQ_BAND, f_lo * pow(f_hi / f_lo, l * depth), q, 0, r->rate);
            }
            r->d[i * r->ch + c] = (float)(x * (1.0 - mix) + mn_biquad_proc(&bq, x) * mix * 1.5);
        }
    }
    return r;
}
/* duck `a` by the level of `key` (in place) */
void mn_sidechain(MnAudio *a, const MnAudio *key, double depth, double thr_lin, double attack_s, double release_s) {
    double ca = exp(-1.0 / (fmax(attack_s, 1e-4) * a->rate)), cr = exp(-1.0 / (fmax(release_s, 1e-3) * a->rate)), env = 0;
    for (size_t i = 0; i < a->frames; i++) {
        double pk = 0;
        if (i < key->frames) for (int c = 0; c < key->ch; c++) { double v = fabs(key->d[i * key->ch + c]); if (v > pk) pk = v; }
        env = pk > env ? ca * env + (1.0 - ca) * pk : cr * env + (1.0 - cr) * pk;
        double e = env / thr_lin; if (e > 1) e = 1;
        float g = (float)(1.0 - depth * e);
        for (int c = 0; c < a->ch; c++) a->d[i * a->ch + c] *= g;
    }
}
/* sidechain(audio, key[, "depth=0.8 threshold=-12 attack=0.004 release=0.14"]) */
static KcValue api_sidechain(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 2, 3, pos, "sidechain(audio, key_audio[, \"depth=0.8 threshold=-12 attack=0.004 release=0.14\"]) -> audio   (duck `audio` whenever `key_audio` is loud, e.g. the kick)");
    MnAudio *x = mn_audio_arg(a[0], pos, "sidechain"), *k = mn_audio_arg(a[1], pos, "sidechain");
    KcValue o = n > 2 ? a[2] : kc_null();
    MnAudio *r = clone(x, pos, "sidechain");
    mn_sidechain(r, k, mn_opt_num(o, "depth", 0.8, pos, "sidechain"), mn_db2lin(mn_opt_num(o, "threshold", -12.0, pos, "sidechain")),
                 mn_opt_num(o, "attack", 0.004, pos, "sidechain"), mn_opt_num(o, "release", 0.14, pos, "sidechain"));
    return mn_audio_wrap(r);
}

static MnAudio *fx_apply(const char *type, const MnAudio *a, KcValue opts, KcPos pos, const char *who);

static const char *PRESET_PHONK[] = { "highpass freq=25", "lowshelf freq=90 gain=2.5", "peak freq=350 gain=-2 q=0.8", "highshelf freq=9000 gain=2",
    "saturate drive=1.8 mix=0.5 bias=0.1", "compressor threshold=-14 ratio=3 attack=0.015 release=0.12 makeup=2", "limiter ceiling=-0.5" };
static const char *PRESET_FUNK[] = { "highpass freq=30", "peak freq=2500 gain=1.5 q=0.9", "highshelf freq=10000 gain=1.5",
    "compressor threshold=-16 ratio=2.5 attack=0.02 release=0.15 makeup=1.5", "limiter ceiling=-0.5" };
static const char *PRESET_CLEAN[] = { "highpass freq=25", "limiter ceiling=-1" };
static const char *PRESET_LOUD[] = { "highpass freq=25", "compressor threshold=-20 ratio=4 attack=0.01 release=0.1 makeup=auto", "saturate drive=1.4 mix=0.3", "limiter ceiling=-0.3 release=0.05" };
static const char *PRESET_LOFI[] = { "highpass freq=60", "lowpass freq=9000", "saturate drive=2.2 mix=0.7 bias=0.15", "vinyl level=0.012", "limiter ceiling=-1" };
static const char *PRESET_TAPE[] = { "saturate drive=1.6 mix=0.6 bias=0.08 tone=14000", "highshelf freq=12000 gain=-1.5", "limiter ceiling=-1" };

MnAudio *mn_master(const MnAudio *a, const char *preset, KcPos pos, const char *who) {
    const char **sp; int ns;
    if (!strcasecmp(preset, "phonk")) { sp = PRESET_PHONK; ns = 7; }
    else if (!strcasecmp(preset, "funk")) { sp = PRESET_FUNK; ns = 5; }
    else if (!strcasecmp(preset, "clean")) { sp = PRESET_CLEAN; ns = 2; }
    else if (!strcasecmp(preset, "loud")) { sp = PRESET_LOUD; ns = 4; }
    else if (!strcasecmp(preset, "lofi")) { sp = PRESET_LOFI; ns = 5; }
    else if (!strcasecmp(preset, "tape")) { sp = PRESET_TAPE; ns = 3; }
    else { kc_error_at(pos, "music_note.%s: unknown master preset '%s' (phonk funk clean loud lofi tape)", who, preset); return NULL; }
    MnAudio *cur = mn_audio_clone(a, pos, who);
    for (int i = 0; i < ns; i++) {
        KcValue o = mn_opts_object(kc_string(sp[i]));
        const char *type = mn_opt_str(o, "type", "gain", pos, who);
        MnAudio *nx = fx_apply(type, cur, o, pos, who);
        mn_audio_free(cur); cur = nx;
    }
    return cur;
}
static MnAudio *fx_master(const MnAudio *a, KcValue o, KcPos pos, const char *who) {
    return mn_master(a, mn_opt_str(o, "preset", "clean", pos, who), pos, who);
}

/* ============================================================ effect table + API */

typedef struct { const char *name; FxFn fn; const char *pos[3]; } Fx;
static const Fx FX[] = {
    { "filter", fx_filter, { "type", "freq", NULL } }, { "lowpass", NULL, { "freq", NULL, NULL } }, { "highpass", NULL, { "freq", NULL, NULL } },
    { "bandpass", NULL, { "freq", "q", NULL } }, { "notch", NULL, { "freq", "q", NULL } },
    { "eq", fx_eq, { "bands", NULL, NULL } }, { "delay", fx_delay, { "time", NULL, NULL } }, { "reverb", fx_reverb, { NULL, NULL, NULL } },
    { "chorus", fx_chorus, { NULL, NULL, NULL } }, { "flanger", fx_flanger, { NULL, NULL, NULL } }, { "phaser", fx_phaser, { NULL, NULL, NULL } },
    { "tremolo", fx_tremolo, { "rate", "depth", NULL } }, { "vibrato", fx_vibrato, { "rate", "depth", NULL } }, { "ring_mod", fx_ringmod, { "freq", "mix", NULL } },
    { "distortion", fx_distortion, { "drive", NULL, NULL } }, { "bitcrush", fx_bitcrush, { "bits", "downsample", NULL } },
    { "compressor", fx_compressor, { "threshold", "ratio", NULL } }, { "limiter", fx_limiter, { "ceiling", NULL, NULL } }, { "gate", fx_gate, { "threshold", NULL, NULL } },
    { "dc_block", fx_dc_block, { NULL, NULL, NULL } }, { "gain", fx_gain, { "db", NULL, NULL } }, { "normalize", fx_normalize, { "peak", NULL, NULL } },
    { "fade", fx_fade, { "in", "out", "curve" } }, { "reverse", fx_reverse, { NULL, NULL, NULL } }, { "pan", fx_pan, { "pos", NULL, NULL } },
    { "width", fx_width, { "amount", NULL, NULL } }, { "to_mono", fx_to_mono, { NULL, NULL, NULL } }, { "to_stereo", fx_to_stereo, { NULL, NULL, NULL } },
    { "swap_channels", fx_swap, { NULL, NULL, NULL } }, { "channel", fx_channel, { "index", NULL, NULL } },
    { "slice", fx_slice, { "start", "end", NULL } }, { "loop", fx_loop, { "times", NULL, NULL } }, { "trim_silence", fx_trim, { "threshold", "pad", NULL } },
    { "resample", fx_resample, { "rate", NULL, NULL } }, { "speed", fx_speed, { "factor", NULL, NULL } },
    { "time_stretch", fx_time_stretch, { "factor", NULL, NULL } }, { "pitch_shift", fx_pitch_shift, { "semitones", NULL, NULL } },
    { "convolve", fx_convolve, { "ir", "mix", NULL } },
    { "saturate", fx_saturate, { "drive", "mix", NULL } }, { "vinyl", fx_vinyl, { "level", NULL, NULL } },
    { "wah", fx_wah, { "rate", "depth", NULL } }, { "master", fx_master, { "preset", NULL, NULL } },
};
#define NFX ((int)(sizeof FX / sizeof FX[0]))

static const Fx *fx_find(const char *name) {
    for (int i = 0; i < NFX; i++) if (!strcasecmp(FX[i].name, name)) return &FX[i];
    return NULL;
}
static MnAudio *fx_apply(const char *type, const MnAudio *a, KcValue opts, KcPos pos, const char *who) {
    const char *real = type;
    KcValue o = mn_opts_object(opts);
    if (strcasecmp(type, "filter") && mn_bq_type_from_name(type) >= 0) {      /* lowpass peak highshelf ... as effect names */
        SETK(o, "type", kc_string(type)); real = "filter";
        if ((!strcasecmp(type, "bandpass") || !strcasecmp(type, "notch")) && !mn_opt_get(o, "q")) SETK(o, "q", kc_float(1.0));
    }
    const Fx *fx = fx_find(real);
    if (!fx || !fx->fn) kc_error_at(pos, "music_note.%s: unknown effect '%s'", who, type);
    return fx->fn(a, o, pos, who);
}

/* generic wrapper used for every named API function */
static KcValue api_fx(const char *name, KcValue *a, int n, KcPos pos) {
    const Fx *fx = fx_find(name);
    MnAudio *in = mn_audio_arg(n > 0 ? a[0] : kc_null(), pos, name);
    KcValue o = kc_object_new();
    int pi = 0;
    for (int i = 1; i < n; i++) {
        if (mn_is_opts(a[i])) { KcValue extra = mn_opts_object(a[i]); KcStructInst *s = extra.as.strct; for (int k = 0; k < s->count; k++) kc_object_set(o, s->names[k], strlen(s->names[k]), s->values[k]); continue; }
        if (pi >= 3 || !fx->pos[pi]) kc_error_at(pos, "music_note.%s: too many positional arguments (pass the rest as an options object)", name);
        kc_object_set(o, fx->pos[pi], strlen(fx->pos[pi]), a[i]); pi++;
    }
    return mn_audio_wrap(fx_apply(name, in, o, pos, name));
}
#define FXFN(nm) static KcValue api_##nm(KcInterp *ip, KcValue *a, int n, KcPos pos) { return api_fx(#nm, a, n, pos); }
FXFN(filter) FXFN(lowpass) FXFN(highpass) FXFN(bandpass) FXFN(notch) FXFN(eq) FXFN(delay) FXFN(reverb) FXFN(chorus) FXFN(flanger) FXFN(phaser)
FXFN(tremolo) FXFN(vibrato) FXFN(distortion) FXFN(bitcrush) FXFN(compressor) FXFN(limiter) FXFN(gate) FXFN(dc_block) FXFN(gain) FXFN(normalize)
FXFN(fade) FXFN(reverse) FXFN(pan) FXFN(width) FXFN(to_mono) FXFN(to_stereo) FXFN(swap_channels) FXFN(channel) FXFN(slice) FXFN(loop)
FXFN(trim_silence) FXFN(resample) FXFN(speed) FXFN(time_stretch) FXFN(pitch_shift) FXFN(convolve)
FXFN(saturate) FXFN(vinyl) FXFN(wah) FXFN(master)
static KcValue api_ring_mod(KcInterp *ip, KcValue *a, int n, KcPos pos) { return api_fx("ring_mod", a, n, pos); }

/* copy(audio) */
static KcValue api_copy(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "copy(audio) -> audio");
    return mn_audio_wrap(mn_audio_clone(mn_audio_arg(a[0], pos, "copy"), pos, "copy"));
}
/* chain(audio, [{type: "reverb", mix: 0.3}, {type: "lowpass", freq: 4000}, ...]) -> audio */
static KcValue api_chain(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 2, 2, pos, "chain(audio, effects[]) -> audio   (each effect a string like \"reverb mix=0.3\" or \"lowpass freq=4000\")");
    MnAudio *cur = mn_audio_arg(a[0], pos, "chain");
    if (a[1].type != VAL_ARRAY) kc_error_at(pos, "music_note.chain: effects must be an array of {type: ...} objects");
    MnAudio *own = NULL;
    for (int i = 0; i < a[1].as.arr->count; i++) {
        KcValue e = a[1].as.arr->items[i];
        if (e.type != VAL_STRING && !mn_is_opts(e)) kc_error_at(pos, "music_note.chain: effect %d must be a string like \"reverb mix=0.3\" (or an object with a 'type')", i);
        const char *t = mn_opt_str(e, "type", NULL, pos, "chain");
        if (!t) kc_error_at(pos, "music_note.chain: effect %d has no 'type'", i);
        MnAudio *nx = fx_apply(t, cur, e, pos, "chain");
        if (own) mn_audio_free(own);
        own = cur = nx;
    }
    if (!own) own = mn_audio_clone(cur, pos, "chain");
    return mn_audio_wrap(own);
}
/* effects() -> names usable in chain() */
static KcValue api_effects(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    KcValue out = kc_array_new();
    for (int i = 0; i < NFX; i++) kc_array_push(out, kc_string(FX[i].name));
    return out;
}

/* ---- multi-buffer ops ---- */
static void convert_into(float *dst, int dch, const MnAudio *s, size_t frames_to_copy, size_t dst_off, float gain) {
    for (size_t i = 0; i < frames_to_copy; i++) for (int c = 0; c < dch; c++) {
        float v = s->ch == dch ? s->d[i * s->ch + c] : s->ch == 1 ? s->d[i] : (c < s->ch ? s->d[i * s->ch + c] : 0.0f);
        dst[(dst_off + i) * dch + c] += v * gain;
    }
}
/* concat(list[, crossfade_seconds]) */
static KcValue api_concat(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "concat(audios[], crossfade_seconds = 0) -> audio");
    if (a[0].type != VAL_ARRAY || a[0].as.arr->count < 1) kc_error_at(pos, "music_note.concat: expected a non-empty array of audio buffers");
    double xf = n > 1 ? mn_num(a[1], pos, "concat", "crossfade_seconds") : 0.0;
    int cnt = a[0].as.arr->count; int ch = 1, rate = 0; size_t total = 0;
    MnAudio **v = kc_malloc((size_t)cnt * sizeof *v);
    for (int i = 0; i < cnt; i++) {
        v[i] = mn_audio_arg(a[0].as.arr->items[i], pos, "concat");
        if (!rate) rate = v[i]->rate; else if (v[i]->rate != rate) { int bad = v[i]->rate; free(v); kc_error_at(pos, "music_note.concat: sample rates differ (%d vs %d); use resample first", rate, bad); }
        if (v[i]->ch > ch) ch = v[i]->ch;
        total += v[i]->frames;
    }
    size_t xfn = (size_t)(xf * rate);
    if (cnt > 1 && xfn) total -= xfn * (size_t)(cnt - 1) < total ? xfn * (size_t)(cnt - 1) : 0;
    MnAudio *r = mn_audio_new(total, ch, rate, pos, "concat");
    size_t off = 0;
    for (int i = 0; i < cnt; i++) {
        size_t nf = v[i]->frames;
        size_t fi = (i > 0 && xfn) ? (xfn < nf ? xfn : nf) : 0;
        for (size_t k = 0; k < nf && off + k < r->frames; k++) {
            float g = 1.0f;
            if (k < fi) g = (float)k / (float)fi;
            if (i + 1 < cnt && xfn && k + xfn >= nf) g *= (float)(nf - k) / (float)xfn;
            for (int c = 0; c < ch; c++) {
                float s = v[i]->ch == ch ? v[i]->d[k * v[i]->ch + c] : v[i]->ch == 1 ? v[i]->d[k] : (c < v[i]->ch ? v[i]->d[k * v[i]->ch + c] : 0.0f);
                r->d[(off + k) * ch + c] += s * g;
            }
        }
        off += nf - (i + 1 < cnt ? (xfn < nf ? xfn : nf) : 0);
    }
    free(v);
    return mn_audio_wrap(r);
}
/* mix(audios[][, gains[]]) */
static KcValue api_mix(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "mix(audios[][, gains[]]) -> audio");
    if (a[0].type != VAL_ARRAY || a[0].as.arr->count < 1) kc_error_at(pos, "music_note.mix: expected a non-empty array of audio buffers");
    int cnt = a[0].as.arr->count, ch = 1, rate = 0; size_t maxn = 0;
    MnAudio **v = kc_malloc((size_t)cnt * sizeof *v);
    for (int i = 0; i < cnt; i++) {
        v[i] = mn_audio_arg(a[0].as.arr->items[i], pos, "mix");
        if (!rate) rate = v[i]->rate; else if (v[i]->rate != rate) { free(v); kc_error_at(pos, "music_note.mix: sample rates differ; use resample first"); }
        if (v[i]->ch > ch) ch = v[i]->ch;
        if (v[i]->frames > maxn) maxn = v[i]->frames;
    }
    MnAudio *r = mn_audio_new(maxn, ch, rate, pos, "mix");
    for (int i = 0; i < cnt; i++) {
        float g = 1.0f;
        if (n > 1 && a[1].type == VAL_ARRAY && i < a[1].as.arr->count) g = (float)mn_db2lin(mn_num(a[1].as.arr->items[i], pos, "mix", "gains[]"));
        convert_into(r->d, ch, v[i], v[i]->frames, 0, g);
    }
    free(v);
    return mn_audio_wrap(r);
}
/* mix_at(base, other, at_seconds[, gain_db = 0]) */
static KcValue api_mix_at(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 3, 4, pos, "mix_at(base, other, at_seconds[, gain_db = 0]) -> audio");
    MnAudio *b = mn_audio_arg(a[0], pos, "mix_at"), *o = mn_audio_arg(a[1], pos, "mix_at");
    if (b->rate != o->rate) kc_error_at(pos, "music_note.mix_at: sample rates differ; use resample first");
    double at = mn_num(a[2], pos, "mix_at", "at_seconds"); if (at < 0) kc_error_at(pos, "music_note.mix_at: at_seconds must be >= 0");
    float g = n > 3 ? (float)mn_db2lin(mn_num(a[3], pos, "mix_at", "gain_db")) : 1.0f;
    size_t off = (size_t)(at * b->rate), need = off + o->frames, total = need > b->frames ? need : b->frames;
    int ch = b->ch > o->ch ? b->ch : o->ch;
    MnAudio *r = mn_audio_new(total, ch, b->rate, pos, "mix_at");
    convert_into(r->d, ch, b, b->frames, 0, 1.0f);
    convert_into(r->d, ch, o, o->frames, off, g);
    return mn_audio_wrap(r);
}
/* impulse_response(seconds[, {decay, rate, channels, predelay, seed, darkening}]) -> audio: synthetic room IR */
static KcValue api_ir(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "impulse_response(seconds[, {decay, rate, channels, predelay, seed}]) -> audio");
    double secs = mn_num(a[0], pos, "impulse_response", "seconds");
    KcValue o = n > 1 ? a[1] : kc_null();
    int rate = (int)mn_opt_int(o, "rate", MN_RATE, pos, "impulse_response"), ch = (int)mn_opt_int(o, "channels", 2, pos, "impulse_response");
    double rt60 = mn_opt_num(o, "decay", secs * 0.7, pos, "impulse_response"), pre = mn_opt_num(o, "predelay", 0.005, pos, "impulse_response");
    if (secs <= 0 || secs > 30) kc_error_at(pos, "music_note.impulse_response: seconds must be 0..30");
    MnAudio *r = mn_audio_new((size_t)(secs * rate), ch, rate, pos, "impulse_response");
    for (int c = 0; c < ch; c++) {
        MnRng rng = { (uint64_t)mn_opt_int(o, "seed", 1, pos, "impulse_response") * 40503u + (uint64_t)c * 7919u + 3 };
        double lp = 0; size_t p0 = (size_t)(pre * rate);
        for (size_t i = p0; i < r->frames; i++) {
            double t = (double)(i - p0) / rate, u = (double)(i - p0) / (double)r->frames;
            double w = mn_rng_f(&rng), cf = 0.15 + 0.82 * u;
            lp += (1.0 - cf) * (w - lp);
            r->d[i * ch + c] = (float)(lp * exp(-6.9078 * t / rt60) * (i - p0 < 64 ? (double)(i - p0) / 64.0 : 1.0));
        }
    }
    return mn_audio_wrap(r);
}

KcModuleEntry mn_dsp_entries[] = {
    { "filter", api_filter }, { "lowpass", api_lowpass }, { "highpass", api_highpass }, { "bandpass", api_bandpass }, { "notch", api_notch },
    { "eq", api_eq }, { "delay", api_delay }, { "reverb", api_reverb }, { "chorus", api_chorus }, { "flanger", api_flanger }, { "phaser", api_phaser },
    { "tremolo", api_tremolo }, { "vibrato", api_vibrato }, { "ring_mod", api_ring_mod }, { "distortion", api_distortion }, { "bitcrush", api_bitcrush },
    { "compressor", api_compressor }, { "limiter", api_limiter }, { "gate", api_gate }, { "dc_block", api_dc_block }, { "gain", api_gain },
    { "normalize", api_normalize }, { "fade", api_fade }, { "reverse", api_reverse }, { "pan", api_pan }, { "width", api_width },
    { "to_mono", api_to_mono }, { "to_stereo", api_to_stereo }, { "swap_channels", api_swap_channels }, { "channel", api_channel },
    { "slice", api_slice }, { "loop", api_loop }, { "trim_silence", api_trim_silence }, { "resample", api_resample }, { "speed", api_speed },
    { "time_stretch", api_time_stretch }, { "pitch_shift", api_pitch_shift }, { "convolve", api_convolve },
    { "copy", api_copy }, { "chain", api_chain }, { "effects", api_effects }, { "concat", api_concat }, { "mix", api_mix }, { "mix_at", api_mix_at },
    { "impulse_response", api_ir }, { "saturate", api_saturate }, { "vinyl", api_vinyl }, { "wah", api_wah }, { "master", api_master },
    { "sidechain", api_sidechain },
};
const int mn_dsp_count = (int)(sizeof mn_dsp_entries / sizeof mn_dsp_entries[0]);
