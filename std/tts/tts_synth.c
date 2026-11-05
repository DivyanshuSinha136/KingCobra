/* KingCobra std.tts — formant synthesizer (Klatt-style).
 *   voicing:     Rosenberg glottal pulse (open quotient, spectral tilt, jitter, shimmer, breathiness)
 *   vocal tract: cascade of five resonators (F1..F5) + nasal pole/zero pair
 *   noise:       aspiration through the cascade; frication/burst through a parallel band-pass (voiced fricatives are
 *                amplitude-modulated by the glottal cycle); whisper voice replaces voicing by aspiration
 *   control:     piecewise-linear parameter tracks built from the planned segments, evaluated every 2 ms
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "tts_internal.h"
#include <math.h>

typedef struct { float *t, *v; int n, cap; } Track;
static void tr_add(Track *tr, float t, float v) {
    if (tr->n == tr->cap) { tr->cap = tr->cap ? tr->cap * 2 : 256; tr->t = realloc(tr->t, (size_t)tr->cap * sizeof(float)); tr->v = realloc(tr->v, (size_t)tr->cap * sizeof(float)); }
    if (tr->n && t < tr->t[tr->n - 1]) t = tr->t[tr->n - 1];
    tr->t[tr->n] = t; tr->v[tr->n] = v; tr->n++;
}
/* evaluate with a cursor (monotone t) */
static float tr_eval(const Track *tr, float t, int *cur) {
    if (!tr->n) return 0;
    if (t <= tr->t[0]) return tr->v[0];
    if (t >= tr->t[tr->n - 1]) return tr->v[tr->n - 1];
    int i = *cur; while (i + 1 < tr->n && tr->t[i + 1] <= t) i++; *cur = i;
    float t0 = tr->t[i], t1 = tr->t[i + 1]; if (t1 - t0 < 1e-4f) return tr->v[i + 1];
    return tr->v[i] + (tr->v[i + 1] - tr->v[i]) * (t - t0) / (t1 - t0);
}
static void tr_free(Track *tr) { free(tr->t); free(tr->v); }

typedef struct { double a, b, c, y1, y2; } Res;
static void res_set(Res *r, double f, double bw, double sr) {
    if (f > sr * 0.47) f = sr * 0.47; if (f < 30) f = 30; if (bw < 20) bw = 20;
    r->c = -exp(-2.0 * M_PI * bw / sr); r->b = 2.0 * exp(-M_PI * bw / sr) * cos(2.0 * M_PI * f / sr); r->a = 1.0 - r->b - r->c;
}
static inline double res_run(Res *r, double x) { double y = r->a * x + r->b * r->y1 + r->c * r->y2; r->y2 = r->y1; r->y1 = y; return y; }
typedef struct { double a, b, c, x1, x2; } Anti;
static void anti_set(Anti *r, double f, double bw, double sr) {
    Res t; res_set(&t, f, bw, sr); r->a = 1.0 / t.a; r->b = -t.b / t.a; r->c = -t.c / t.a;
}
static inline double anti_run(Anti *r, double x) { double y = r->a * x + r->b * r->x1 + r->c * r->x2; r->x2 = r->x1; r->x1 = x; return y; }

/* RBJ band-pass (zero gain at DC and Nyquist): the frication / burst shaper */
typedef struct { double b0, a1, a2, x1, x2, y1, y2; } Bpf;
static void bpf_set(Bpf *f, double fc, double bw, double sr) {
    if (fc > sr * 0.45) fc = sr * 0.45; if (fc < 200) fc = 200; if (bw < 200) bw = 200;
    double Q = fc / bw; if (Q < 0.5) Q = 0.5; if (Q > 8) Q = 8;
    double w0 = 2.0 * M_PI * fc / sr, al = sin(w0) / (2.0 * Q), a0 = 1.0 + al;
    f->b0 = al / a0; f->a1 = -2.0 * cos(w0) / a0; f->a2 = (1.0 - al) / a0;
}
static inline double bpf_run(Bpf *f, double x) {
    double y = f->b0 * (x - f->x2) - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x; f->y2 = f->y1; f->y1 = y; return y;
}
static inline double dbamp(double db) { return db <= 1.0 ? 0.0 : pow(10.0, (db - 60.0) / 20.0); }
static inline double glottal(double ph, double oq) {
    double tp = oq * 0.7, tn = oq * 0.3;
    if (ph < tp) return 0.5 * (1.0 - cos(M_PI * ph / tp));
    if (ph < oq) return cos(M_PI * 0.5 * (ph - tp) / tn);
    return 0.0;
}

#ifndef FRIC_GAIN
#define FRIC_GAIN 5.5
#endif
MnAudio *tts_synth(const TtsPlan *pl, const TtsOpts *o, KcPos pos, const char *who) {
    const double sr = o->sr;
    size_t total = (size_t)(pl->total_ms * sr / 1000.0) + 16;
    MnAudio *au = mn_audio_new(total, 1, o->sr, pos, who);
    int step = (int)(sr * 0.002); if (step < 8) step = 8;
    int nfr = (int)(total / (size_t)step) + 2;

    /* ---- parameter tracks ---- */
    enum { F1, F2, F3, B1, B2, B3, AV, AF, AH, FC, FBW, NAS, FZ, NT };
    Track T[NT]; memset(T, 0, sizeof T);
    for (int i = 0; i < pl->n; i++) {
        const TtsSeg *s = &pl->s[i]; float t0 = s->t0, t1 = s->t0 + s->dur;
        float r = s->soft ? 12.0f : 4.0f; if (r > s->dur * 0.5f) r = s->dur * 0.5f;
        float rn = 10.0f; if (rn > s->dur * 0.5f) rn = s->dur * 0.5f;
        if (!(s->flags & SEGF_NOFORM)) {
            float a = s->tin, b = s->tout; if (a + b > s->dur) { a = s->dur * 0.5f; b = s->dur * 0.5f; }
            for (int k = 0; k < 3; k++) { tr_add(&T[F1 + k], t0 + a, s->fS[k]); tr_add(&T[F1 + k], t1 - b, s->fE[k]); tr_add(&T[B1 + k], t0 + 1, s->bw[k]); tr_add(&T[B1 + k], t1 - 1, s->bw[k]); }
            tr_add(&T[FC], t0 + 1, s->fc); tr_add(&T[FC], t1 - 1, s->fc); tr_add(&T[FBW], t0 + 1, s->fbw); tr_add(&T[FBW], t1 - 1, s->fbw);
            tr_add(&T[FZ], t0 + 1, s->fz); tr_add(&T[FZ], t1 - 1, s->fz);
        }
        tr_add(&T[AV], t0 + r, s->avS); tr_add(&T[AV], t1 - r, s->avE);
        tr_add(&T[AF], t0 + r, s->afS); tr_add(&T[AF], t1 - r, s->afE);
        tr_add(&T[AH], t0 + r, s->ahS); tr_add(&T[AH], t1 - r, s->ahE);
        tr_add(&T[NAS], t0 + rn, s->nasal); tr_add(&T[NAS], t1 - rn, s->nasal);
    }
    /* ---- synthesis ---- */
    MnRng rng = { (uint64_t)o->seed * 0x9E3779B97F4A7C15ull + 0x1234567ull }, rng2 = { (uint64_t)o->seed * 0xD1B54A32D192ED03ull + 0x7654321ull };
    int cur[NT] = { 0 }; int cf0 = 0, cseg = 0;
    Res R[5], RN; Bpf RF, RF2; Anti AN; memset(R, 0, sizeof R); memset(&RN, 0, sizeof RN); memset(&RF, 0, sizeof RF); memset(&RF2, 0, sizeof RF2); memset(&AN, 0, sizeof AN);
    double phase = 0, gprev = 0, vt = 0, shim = 1.0, jit = 1.0, flutter = 0, flutter_t = 0, pav = 0, paf = 0, pah = 0, pvol = 1.0, pnas = 0;
    double dcx = 0, dcy = 0; float *out = au->d; size_t pos_s = 0;
    TtsStyle sm; tts_style_neutral(&sm); bool sm_init = false;
    double sumsq = 0, sumvol = 0; size_t nz = 0, nvol = 0;
    for (int fr = 0; fr < nfr && pos_s < total; fr++) {
        float tms = (float)((double)pos_s * 1000.0 / sr);
        while (cseg + 1 < pl->n && pl->s[cseg + 1].t0 <= tms) cseg++;
        const TtsSeg *sg = &pl->s[cseg < pl->n ? cseg : pl->n - 1];
        const TtsVoice *V = tts_voice_at(sg->voice);
        const TtsStyle *tg = &pl->sty[sg->sty < pl->nsty ? sg->sty : 0];
        if (!sm_init) { sm = *tg; sm_init = true; }
        else { const float a = 0.12f;
            #define SMF(f) sm.f += (tg->f - sm.f) * a
            SMF(breath); SMF(whisper); SMF(tilt); SMF(oq); SMF(jitter); SMF(shimmer); SMF(tremor); SMF(tremor_hz); SMF(creak); SMF(formant); SMF(artic); SMF(amod);
            #undef SMF
        }
        double tract = V->tract * sm.formant, bws = V->bw;
        double f[3], bw[3];
        for (int k = 0; k < 3; k++) { f[k] = tr_eval(&T[F1 + k], tms, &cur[F1 + k]) * tract; bw[k] = tr_eval(&T[B1 + k], tms, &cur[B1 + k]) * bws; }
        double av = tr_eval(&T[AV], tms, &cur[AV]), af = tr_eval(&T[AF], tms, &cur[AF]), ah = tr_eval(&T[AH], tms, &cur[AH]);
        double fc = tr_eval(&T[FC], tms, &cur[FC]) * (0.5 + 0.5 * tract), fbw = tr_eval(&T[FBW], tms, &cur[FBW]), nas = tr_eval(&T[NAS], tms, &cur[NAS]), fz = tr_eval(&T[FZ], tms, &cur[FZ]) * tract;
        if (f[0] < 120) f[0] = 120; if (f[1] < f[0] + 150) f[1] = f[0] + 150; if (f[2] < f[1] + 200) f[2] = f[1] + 200;
        /* F0 from anchors (cosine interpolation in semitones) */
        double st = 0;
        if (pl->nf0) {
            if (tms <= pl->f0t[0]) st = pl->f0v[0]; else if (tms >= pl->f0t[pl->nf0 - 1]) st = pl->f0v[pl->nf0 - 1];
            else { int i = cf0; while (i + 1 < pl->nf0 && pl->f0t[i + 1] <= tms) i++; cf0 = i; float d = pl->f0t[i + 1] - pl->f0t[i]; double u = d > 1e-3f ? (tms - pl->f0t[i]) / d : 1.0; u = 0.5 - 0.5 * cos(M_PI * u); st = pl->f0v[i] + (pl->f0v[i + 1] - pl->f0v[i]) * u; }
        }
        double base = (sg->voice == o->voice && o->f0 > 0) ? o->f0 : V->f0;
        double f0 = base * pow(2.0, (st + o->pitch_st) / 12.0);
        if (V->robot) f0 = base * pow(2.0, o->pitch_st / 12.0);
        if (sm.tremor > 0.001f) f0 *= pow(2.0, sm.tremor * sin(2.0 * M_PI * sm.tremor_hz * tms / 1000.0) / 12.0);
        if (f0 < 40) f0 = 40; if (f0 > 900) f0 = 900;
        double whisper = V->whisper ? 1.0 : sm.whisper;
        double avl = dbamp(av), ahl = dbamp(ah), afl = dbamp(af) * 0.9;
        if (whisper > 0) { ahl += avl * 1.5 * whisper; avl *= (1.0 - whisper); }
        afl *= sm.artic;
        double vol = sg->vol;
        if (sm.amod > 0.001f) vol *= 1.0 - sm.amod * 0.5 * (1.0 - cos(2.0 * M_PI * (sm.tremor_hz > 3 ? sm.tremor_hz : 6.5) * tms / 1000.0));
        /* coefficients for this frame */
        for (int k = 0; k < 3; k++) res_set(&R[k], f[k], bw[k], sr);
        res_set(&R[3], 3300.0 * tract, 250.0 * bws, sr); res_set(&R[4], 3750.0 * tract, 300.0 * bws, sr);
        res_set(&RN, 270.0, 90.0, sr); if (nas > 0.01 && fz > 100) anti_set(&AN, fz, 110.0, sr);
        bpf_set(&RF, fc, fbw, sr); RF2.b0 = RF.b0; RF2.a1 = RF.a1; RF2.a2 = RF.a2;
        double fnorm = sqrt(3000.0 / (fbw < 600 ? 600 : fbw));
        double oq = V->oq + sm.oq, tilt = V->tilt + o->tilt + sm.tilt, breath = V->breath + o->breath + sm.breath;
        if (oq < 0.3) oq = 0.3; if (oq > 0.9) oq = 0.9; if (breath < 0) breath = 0;
        double jit_a = V->jitter + sm.jitter, shim_a = V->shimmer + sm.shimmer, creak_a = sm.creak; if (tilt > 0.95) tilt = 0.95; if (tilt < 0) tilt = 0;
        int n = step; if (pos_s + (size_t)n > total) n = (int)(total - pos_s);
        double dav = (avl - pav) / n, dah = (ahl - pah) / n, daf = (afl - paf) / n, dvol = (vol - pvol) / n, dnas = (nas - pnas) / n;
        double ainc = f0 / sr;
        for (int i = 0; i < n; i++) {
            pav += dav; pah += dah; paf += daf; pvol += dvol; pnas += dnas;
            phase += ainc;
            if (phase >= 1.0) { phase -= 1.0; shim = 1.0 + shim_a * mn_rng_f(&rng2); jit = 1.0 + jit_a * mn_rng_f(&rng2);
                if (creak_a > 0.01 && (0.5 + 0.5 * mn_rng_f(&rng2)) < creak_a * 0.65) { jit *= 0.5; shim *= 0.5; } flutter_t += 0.017; flutter = 0.004 * (sin(flutter_t * 7.3) + sin(flutter_t * 12.7) + sin(flutter_t * 19.1)); phase = phase * 1.0; }
            double fj = f0 * jit * (1.0 + (V->robot ? 0 : flutter));
            ainc = fj / sr;
            double g = glottal(phase, oq);
            double dg = (g - gprev) * (sr / fj) / 6.5; gprev = g;
            double vs = dg * shim;
            vt = (1.0 - tilt) * vs + tilt * vt;
            double nzv = mn_rng_f(&rng);
            double src = vt * pav + nzv * pah + nzv * breath * pav * (0.6 - 0.4 * g) * 0.35;
            double x = src;
            if (pnas > 0.01) { double xa = anti_run(&AN, x); double xm = (1.0 - pnas) * x + pnas * xa; double xp = res_run(&RN, xm); x = xm + pnas * 0.6 * (xp - xm) * 0.5; } else { AN.x2 = AN.x1; AN.x1 = x; }
            for (int k = 0; k < 5; k++) x = res_run(&R[k], x);
            double fr = 0;
            if (paf > 1e-6) {
                double nf = mn_rng_f(&rng2);
                double mod = (sg->vmod) ? (0.25 + 0.75 * (1.0 - g)) : 1.0;
                fr = bpf_run(&RF2, bpf_run(&RF, nf * paf * mod)) * FRIC_GAIN * fnorm;
            } else { bpf_run(&RF2, bpf_run(&RF, 0.0)); }
            double y = (x + fr) * pvol * 0.55;
            double yd = y - dcx + 0.995 * dcy; dcx = y; dcy = yd;
            out[pos_s++] = (float)yd; sumsq += yd * yd; nz++;
            if (yd * yd > 1e-6) { sumvol += pvol * pvol; nvol++; }
        }
    }
    for (int k = 0; k < NT; k++) tr_free(&T[k]);
    /* ---- level: RMS-normalise, soft-limit ---- */
    double rms = nz ? sqrt(sumsq / (double)nz) : 0.0;
    double mv = nvol ? sqrt(sumvol / (double)nvol) : 1.0; if (mv < 0.35) mv = 0.35; if (mv > 2.0) mv = 2.0;
    double target = 0.13 * o->volume * mv;
    double gain = rms > 1e-9 ? target / rms : 1.0;
    for (size_t i = 0; i < au->frames; i++) { double v = out[i] * gain; if (v > 0.8 || v < -0.8) { double a = fabs(v), sgn = v < 0 ? -1.0 : 1.0; v = sgn * (0.8 + 0.19 * tanh((a - 0.8) / 0.19)); } out[i] = (float)v; }
    if (o->reverb > 0.001) mn_reverb(au, 0.55, 0.5, o->reverb * 0.5, 0.7, 0.01, 0.5, pos, who);
    return au;
}
