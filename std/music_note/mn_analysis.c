/* KingCobra std.music_note — analysis: stats, sample access, FFT, spectrum, pitch (YIN), onsets, tempo.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mn_internal.h"

#define SETK(o, k, v) kc_object_set((o), (k), strlen(k), (v))
#define MAX_RET 4000000

static double window_at(int kind, size_t i, size_t n) {
    double x = n > 1 ? (double)i / (double)(n - 1) : 0.0;
    switch (kind) {
        case 1: return 0.54 - 0.46 * cos(MN_TAU * x);                                              /* hamming */
        case 2: return 0.42 - 0.5 * cos(MN_TAU * x) + 0.08 * cos(2 * MN_TAU * x);                  /* blackman */
        case 3: return 1.0;                                                                         /* rect */
        case 4: return 1.0 - fabs(2.0 * x - 1.0);                                                   /* triangle */
        default: return 0.5 - 0.5 * cos(MN_TAU * x);                                                /* hann */
    }
}
static int window_kind(const char *s, KcPos pos, const char *who) {
    if (!strcasecmp(s, "hann") || !strcasecmp(s, "hanning")) return 0;
    if (!strcasecmp(s, "hamming")) return 1;
    if (!strcasecmp(s, "blackman")) return 2;
    if (!strcasecmp(s, "rect") || !strcasecmp(s, "rectangular") || !strcasecmp(s, "none")) return 3;
    if (!strcasecmp(s, "triangle") || !strcasecmp(s, "bartlett")) return 4;
    kc_error_at(pos, "music_note.%s: unknown window '%s' (hann hamming blackman rect triangle)", who, s);
    return 0;
}
static size_t pow2_ge(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }
static float mono_at(const MnAudio *a, size_t i) {
    if (i >= a->frames) return 0.0f;
    float s = 0; for (int c = 0; c < a->ch; c++) s += a->d[i * a->ch + c];
    return s / (float)a->ch;
}

/* info(audio) -> {frames, seconds, rate, channels} */
static KcValue an_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "info(audio) -> {frames, seconds, rate, channels}");
    MnAudio *x = mn_audio_arg(a[0], pos, "info");
    KcValue o = kc_object_new();
    SETK(o, "frames", kc_int((int64_t)x->frames)); SETK(o, "seconds", kc_float((double)x->frames / x->rate));
    SETK(o, "rate", kc_int(x->rate)); SETK(o, "channels", kc_int(x->ch));
    return o;
}
/* stats(audio) -> {peak, peak_db, rms, rms_db, dc, crest_db, clipped, silent} */
static KcValue an_stats(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "stats(audio) -> {peak, peak_db, rms, rms_db, dc, crest_db, clipped, silent}");
    MnAudio *x = mn_audio_arg(a[0], pos, "stats");
    size_t tot = x->frames * (size_t)x->ch; double pk = 0, sq = 0, sum = 0; int64_t clipped = 0;
    for (size_t i = 0; i < tot; i++) { double v = x->d[i], av = fabs(v); if (av > pk) pk = av; sq += v * v; sum += v; if (av >= 0.999) clipped++; }
    double rms = tot ? sqrt(sq / (double)tot) : 0.0;
    KcValue o = kc_object_new();
    SETK(o, "peak", kc_float(pk)); SETK(o, "peak_db", kc_float(mn_lin2db(pk))); SETK(o, "rms", kc_float(rms)); SETK(o, "rms_db", kc_float(mn_lin2db(rms)));
    SETK(o, "dc", kc_float(tot ? sum / (double)tot : 0.0)); SETK(o, "crest_db", kc_float(mn_lin2db(pk) - mn_lin2db(rms)));
    SETK(o, "clipped", kc_int(clipped)); SETK(o, "silent", kc_bool(pk < 1e-6));
    return o;
}
/* samples(audio[, {channel = -1 (mono mix), start = 0 (s), count, step = 1}]) -> float[] */
static KcValue an_samples(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "samples(audio[, {channel, start, count, step}]) -> float[]");
    MnAudio *x = mn_audio_arg(a[0], pos, "samples"); KcValue o = n > 1 ? a[1] : kc_null();
    int64_t ch = mn_opt_int(o, "channel", -1, pos, "samples"), step = mn_opt_int(o, "step", 1, pos, "samples");
    size_t st = (size_t)(mn_opt_num(o, "start", 0.0, pos, "samples") * x->rate);
    int64_t cnt = mn_opt_int(o, "count", (int64_t)x->frames, pos, "samples");
    if (ch < -1 || ch >= x->ch) kc_error_at(pos, "music_note.samples: channel out of range");
    if (step < 1) kc_error_at(pos, "music_note.samples: step must be >= 1");
    KcValue out = kc_array_new(); int64_t emitted = 0;
    for (size_t i = st; i < x->frames && cnt-- > 0; i += (size_t)step) {
        if (++emitted > MAX_RET) kc_error_at(pos, "music_note.samples: too many samples requested (limit %d); use start/count/step", MAX_RET);
        kc_array_push(out, kc_float(ch < 0 ? mono_at(x, i) : x->d[i * x->ch + ch]));
    }
    return out;
}
/* from_samples(samples[][, rate = 44100[, channels = 1]]) -> audio */
static KcValue an_from_samples(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 3, pos, "from_samples(samples[][, rate = 44100[, channels = 1]]) -> audio   (samples are -1..1, interleaved when channels > 1)");
    if (a[0].type != VAL_ARRAY) kc_error_at(pos, "music_note.from_samples: expected an array of numbers");
    int rate = n > 1 ? (int)mn_int(a[1], pos, "from_samples", "rate") : MN_RATE, ch = n > 2 ? (int)mn_int(a[2], pos, "from_samples", "channels") : 1;
    int cnt = a[0].as.arr->count;
    if (ch < 1 || cnt % ch) kc_error_at(pos, "music_note.from_samples: %d samples do not divide into %d channels", cnt, ch);
    MnAudio *r = mn_audio_new((size_t)(cnt / ch), ch, rate, pos, "from_samples");
    for (int i = 0; i < cnt; i++) r->d[i] = (float)mn_num(a[0].as.arr->items[i], pos, "from_samples", "sample");
    return mn_audio_wrap(r);
}
/* waveform(audio, points) -> {min: float[], max: float[]}   — for drawing */
static KcValue an_waveform(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 2, 2, pos, "waveform(audio, points) -> {min[], max[]}");
    MnAudio *x = mn_audio_arg(a[0], pos, "waveform"); int64_t pts = mn_int(a[1], pos, "waveform", "points");
    if (pts < 1 || pts > 100000) kc_error_at(pos, "music_note.waveform: points must be 1..100000");
    KcValue mins = kc_array_new(), maxs = kc_array_new();
    for (int64_t p = 0; p < pts; p++) {
        size_t s0 = (size_t)((double)p * (double)x->frames / (double)pts), s1 = (size_t)((double)(p + 1) * (double)x->frames / (double)pts);
        if (s1 <= s0) s1 = s0 + 1;
        float lo = 0, hi = 0; bool f = true;
        for (size_t i = s0; i < s1 && i < x->frames; i++) { float v = mono_at(x, i); if (f || v < lo) lo = v; if (f || v > hi) hi = v; f = false; }
        kc_array_push(mins, kc_float(lo)); kc_array_push(maxs, kc_float(hi));
    }
    KcValue o = kc_object_new(); SETK(o, "min", mins); SETK(o, "max", maxs);
    return o;
}
/* window(kind, n) -> float[] */
static KcValue an_window(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 2, 2, pos, "window(kind, n) -> float[]   (hann hamming blackman rect triangle)");
    int k = window_kind(mn_str(a[0], pos, "window", "kind"), pos, "window"); int64_t N = mn_int(a[1], pos, "window", "n");
    if (N < 1 || N > 1 << 22) kc_error_at(pos, "music_note.window: n must be 1..4194304");
    KcValue out = kc_array_new();
    for (int64_t i = 0; i < N; i++) kc_array_push(out, kc_float(window_at(k, (size_t)i, (size_t)N)));
    return out;
}
/* fft(samples[]) -> {re[], im[], mag[]}   (zero-padded to a power of two) */
static KcValue an_fft(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "fft(samples[][, inverse = false]) -> {re[], im[], mag[]}   (zero-padded to a power of two; with inverse=true pass an object {re, im})");
    bool inv = n > 1 && kc_truthy(a[1]);
    size_t cnt; double *re, *im;
    if (inv) {
        KcValue *rv = mn_opt_get(a[0], "re"), *iv = mn_opt_get(a[0], "im");
        if (!rv || !iv || rv->type != VAL_ARRAY || iv->type != VAL_ARRAY) kc_error_at(pos, "music_note.fft: inverse needs an object {re: [...], im: [...]}");
        cnt = (size_t)rv->as.arr->count; if (!cnt || (size_t)iv->as.arr->count != cnt) kc_error_at(pos, "music_note.fft: re and im must have the same non-zero length");
        size_t N = pow2_ge(cnt); if (N != cnt) kc_error_at(pos, "music_note.fft: inverse length must be a power of two");
        re = kc_malloc(N * sizeof(double)); im = kc_malloc(N * sizeof(double));
        for (size_t i = 0; i < N; i++) { re[i] = mn_num(rv->as.arr->items[i], pos, "fft", "re[]"); im[i] = mn_num(iv->as.arr->items[i], pos, "fft", "im[]"); }
        mn_fft(re, im, N, true); cnt = N;
    } else {
        if (a[0].type != VAL_ARRAY || a[0].as.arr->count < 1) kc_error_at(pos, "music_note.fft: expected a non-empty array of numbers");
        cnt = (size_t)a[0].as.arr->count; size_t N = pow2_ge(cnt);
        if (N > (1u << 20)) kc_error_at(pos, "music_note.fft: at most 1048576 points");
        re = kc_malloc(N * sizeof(double)); im = kc_malloc(N * sizeof(double)); memset(re, 0, N * sizeof(double)); memset(im, 0, N * sizeof(double));
        for (size_t i = 0; i < cnt; i++) re[i] = mn_num(a[0].as.arr->items[i], pos, "fft", "sample");
        mn_fft(re, im, N, false); cnt = N;
    }
    KcValue R = kc_array_new(), I = kc_array_new(), M = kc_array_new();
    for (size_t i = 0; i < cnt; i++) { kc_array_push(R, kc_float(re[i])); kc_array_push(I, kc_float(im[i])); kc_array_push(M, kc_float(sqrt(re[i] * re[i] + im[i] * im[i]))); }
    free(re); free(im);
    KcValue o = kc_object_new(); SETK(o, "re", R); SETK(o, "im", I); SETK(o, "mag", M);
    return o;
}
/* spectrum(audio[, {size = 4096, start = 0, window = "hann"}]) -> {freqs[], mags[], db[], peak_freq, peak_mag} */
static KcValue an_spectrum(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "spectrum(audio[, {size = 4096, start = 0 (seconds), window = \"hann\"}]) -> {freqs[], mags[], db[], peak_freq, peak_mag}");
    MnAudio *x = mn_audio_arg(a[0], pos, "spectrum"); KcValue o = n > 1 ? a[1] : kc_null();
    int64_t size = mn_opt_int(o, "size", 4096, pos, "spectrum");
    if (size < 16 || size > (1 << 20) || (size & (size - 1))) kc_error_at(pos, "music_note.spectrum: size must be a power of two from 16 to 1048576");
    int wk = window_kind(mn_opt_str(o, "window", "hann", pos, "spectrum"), pos, "spectrum");
    size_t st = (size_t)(mn_opt_num(o, "start", 0.0, pos, "spectrum") * x->rate), N = (size_t)size;
    double *re = kc_malloc(N * sizeof(double)), *im = kc_malloc(N * sizeof(double)), wsum = 0;
    for (size_t i = 0; i < N; i++) { double w = window_at(wk, i, N); wsum += w; re[i] = mono_at(x, st + i) * w; im[i] = 0; }
    mn_fft(re, im, N, false);
    size_t H = N / 2; double *mag = kc_malloc((H + 1) * sizeof(double));
    KcValue F = kc_array_new(), M = kc_array_new(), D = kc_array_new(); size_t pk = 1; double pm = 0;
    for (size_t k = 0; k <= H; k++) {
        double m = sqrt(re[k] * re[k] + im[k] * im[k]) * (k == 0 || k == H ? 1.0 : 2.0) / wsum; mag[k] = m;
        if (k >= 1 && m > pm) { pm = m; pk = k; }
        kc_array_push(F, kc_float((double)k * x->rate / (double)N)); kc_array_push(M, kc_float(m)); kc_array_push(D, kc_float(mn_lin2db(m)));
    }
    double pf = (double)pk * x->rate / (double)N;
    if (pk > 1 && pk < H) {      /* parabolic interpolation on log magnitude */
        double l = log(mag[pk - 1] + 1e-20), c = log(mag[pk] + 1e-20), r = log(mag[pk + 1] + 1e-20), den = l - 2 * c + r;
        if (den < 0) pf = ((double)pk + 0.5 * (l - r) / den) * x->rate / (double)N;
    }
    free(re); free(im); free(mag);
    KcValue res = kc_object_new();
    SETK(res, "freqs", F); SETK(res, "mags", M); SETK(res, "db", D); SETK(res, "peak_freq", kc_float(pf)); SETK(res, "peak_mag", kc_float(pm));
    return res;
}
/* pitch(audio[, {min = 50, max = 2000, start = 0, size = 4096, threshold = 0.15}]) -> {freq, confidence, voiced, note, midi, cents} */
static KcValue an_pitch(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "pitch(audio[, {min = 50, max = 2000, start = 0 (s), size = 4096, threshold = 0.15}]) -> {freq, confidence, voiced, note, midi, cents}");
    MnAudio *x = mn_audio_arg(a[0], pos, "pitch"); KcValue o = n > 1 ? a[1] : kc_null();
    double fmin = mn_opt_num(o, "min", 50, pos, "pitch"), fmax = mn_opt_num(o, "max", 2000, pos, "pitch"), thr = mn_opt_num(o, "threshold", 0.15, pos, "pitch");
    size_t st = (size_t)(mn_opt_num(o, "start", 0.0, pos, "pitch") * x->rate); int64_t size = mn_opt_int(o, "size", 4096, pos, "pitch");
    if (fmin < 20 || fmax > x->rate / 2.0 || fmin >= fmax) kc_error_at(pos, "music_note.pitch: need 20 <= min < max <= half the sample rate");
    if (size < 256 || size > 65536) kc_error_at(pos, "music_note.pitch: size must be 256..65536");
    size_t tmax = (size_t)(x->rate / fmin) + 1, tmin = (size_t)(x->rate / fmax); if (tmin < 2) tmin = 2;
    size_t H = (size_t)size; if (tmax >= H) kc_error_at(pos, "music_note.pitch: size too small for min=%g Hz at %d Hz (need > %zu)", fmin, x->rate, tmax);
    float *fr = kc_malloc((H + tmax) * sizeof(float)); for (size_t i = 0; i < H + tmax; i++) fr[i] = mono_at(x, st + i);
    double *d = kc_malloc((tmax + 1) * sizeof(double)); d[0] = 1.0;
    double run = 0;
    for (size_t t = 1; t <= tmax; t++) {
        double s = 0; for (size_t j = 0; j < H; j++) { double df = (double)fr[j] - fr[j + t]; s += df * df; }
        run += s; d[t] = run > 0 ? s * (double)t / run : 1.0;
    }
    size_t best = 0;
    for (size_t t = tmin; t <= tmax; t++) { if (d[t] < thr) { while (t + 1 <= tmax && d[t + 1] < d[t]) t++; best = t; break; } }
    if (!best) { double m = 1e30; for (size_t t = tmin; t <= tmax; t++) if (d[t] < m) { m = d[t]; best = t; } }
    double tau = (double)best;
    if (best > 1 && best < tmax) { double s0 = d[best - 1], s1 = d[best], s2 = d[best + 1], den = s0 - 2 * s1 + s2; if (den > 1e-12) tau += 0.5 * (s0 - s2) / den; }
    double conf = 1.0 - d[best]; if (conf < 0) conf = 0;
    double f = x->rate / tau; bool voiced = conf >= 0.7;
    free(fr); free(d);
    KcValue res = kc_object_new();
    SETK(res, "freq", kc_float(voiced ? f : 0.0)); SETK(res, "confidence", kc_float(conf)); SETK(res, "voiced", kc_bool(voiced));
    if (voiced) {
        double m = 69.0 + 12.0 * log2(f / 440.0); long nm = lround(m); char nb[16];
        SETK(res, "note", kc_string(mn_midi_name((int)nm, false, nb))); SETK(res, "midi", kc_int(nm)); SETK(res, "cents", kc_float((m - (double)nm) * 100.0));
    } else { SETK(res, "note", kc_string("")); SETK(res, "midi", kc_int(-1)); SETK(res, "cents", kc_float(0)); }
    return res;
}

/* spectral-flux onset-strength envelope */
static double *flux_env(const MnAudio *x, size_t hop, size_t size, size_t *nframes) {
    size_t nf = x->frames > size ? (x->frames - size) / hop + 1 : 1;
    double *env = kc_malloc(nf * sizeof(double)), *re = kc_malloc(size * sizeof(double)), *im = kc_malloc(size * sizeof(double));
    double *prev = kc_malloc((size / 2) * sizeof(double)); memset(prev, 0, (size / 2) * sizeof(double));
    for (size_t f = 0; f < nf; f++) {
        for (size_t i = 0; i < size; i++) { re[i] = mono_at(x, f * hop + i) * window_at(0, i, size); im[i] = 0; }
        mn_fft(re, im, size, false);
        double flux = 0;
        for (size_t k = 1; k < size / 2; k++) {
            double m = log1p(50.0 * sqrt(re[k] * re[k] + im[k] * im[k]) / (double)size * 8.0), df = m - prev[k];
            if (df > 0) flux += df;
            prev[k] = m;
        }
        env[f] = flux;
    }
    free(re); free(im); free(prev);
    *nframes = nf; return env;
}
/* onsets(audio[, {sensitivity = 1.4, min_gap = 0.05}]) -> seconds[] */
static KcValue an_onsets(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "onsets(audio[, {sensitivity = 1.4, min_gap = 0.05}]) -> seconds[]");
    MnAudio *x = mn_audio_arg(a[0], pos, "onsets"); KcValue o = n > 1 ? a[1] : kc_null();
    double sens = mn_opt_num(o, "sensitivity", 1.4, pos, "onsets"), gap = mn_opt_num(o, "min_gap", 0.05, pos, "onsets");
    size_t hop = 512, size = 1024, nf; double *env = flux_env(x, hop, size, &nf);
    KcValue out = kc_array_new(); long last = -1000000; long gapf = (long)(gap * x->rate / (double)hop);
    for (size_t f = 0; f + 1 < nf; f++) {
        size_t lo = f > 8 ? f - 8 : 0, hi = f + 8 < nf ? f + 8 : nf - 1; double mean = 0; for (size_t k = lo; k <= hi; k++) mean += env[k]; mean /= (double)(hi - lo + 1);
        if (env[f] > (f ? env[f - 1] : 0.0) && env[f] >= env[f + 1] && env[f] > mean * sens + 0.05 && (long)f - last > gapf) {
            kc_array_push(out, kc_float((double)f * hop / x->rate)); last = (long)f;
        }
    }
    free(env); return out;
}
/* bpm(audio[, {min = 60, max = 200}]) -> {bpm, confidence} */
static KcValue an_bpm(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "bpm(audio[, {min = 60, max = 200}]) -> {bpm, confidence}");
    MnAudio *x = mn_audio_arg(a[0], pos, "bpm"); KcValue o = n > 1 ? a[1] : kc_null();
    double bmin = mn_opt_num(o, "min", 60, pos, "bpm"), bmax = mn_opt_num(o, "max", 200, pos, "bpm");
    if (bmin < 20 || bmax > 400 || bmin >= bmax) kc_error_at(pos, "music_note.bpm: need 20 <= min < max <= 400");
    size_t hop = 256, size = 1024, nf; double *env = flux_env(x, hop, size, &nf);
    double fr = (double)x->rate / (double)hop;
    if (nf < (size_t)(fr * 4)) { free(env); kc_error_at(pos, "music_note.bpm: need at least a few seconds of audio"); }
    double mean = 0; for (size_t i = 0; i < nf; i++) mean += env[i]; mean /= (double)nf;
    for (size_t i = 0; i < nf; i++) env[i] -= mean;
    size_t lmin = (size_t)(60.0 * fr / bmax), lmax = (size_t)(60.0 * fr / bmin) + 1; if (lmax >= nf / 2) lmax = nf / 2 - 1;
    double r0 = 0; for (size_t i = 0; i < nf; i++) r0 += env[i] * env[i];
    double best = -1e30, bl = (double)lmin; double *ac = kc_malloc((lmax + 2) * sizeof(double));
    for (size_t l = lmin; l <= lmax; l++) {
        double s = 0; for (size_t i = 0; i + l < nf; i++) s += env[i] * env[i + l];
        s /= (double)(nf - l);
        double bpm = 60.0 * fr / (double)l, w = exp(-0.5 * pow(log2(bpm / 120.0) / 1.2, 2.0));    /* gentle prior towards 120 */
        ac[l] = s; s *= (0.6 + 0.4 * w);
        if (s > best) { best = s; bl = (double)l; }
    }
    size_t li = (size_t)bl;
    if (li > lmin && li < lmax) { double s0 = ac[li - 1], s1 = ac[li], s2 = ac[li + 1], den = s0 - 2 * s1 + s2; if (den < -1e-12) bl += 0.5 * (s0 - s2) / den; }
    double conf = r0 > 0 ? ac[li] / (r0 / (double)nf) : 0.0; if (conf < 0) conf = 0; if (conf > 1) conf = 1;
    free(env); free(ac);
    KcValue res = kc_object_new(); SETK(res, "bpm", kc_float(60.0 * fr / bl)); SETK(res, "confidence", kc_float(conf));
    return res;
}

KcModuleEntry mn_analysis_entries[] = {
    { "info", an_info }, { "stats", an_stats }, { "samples", an_samples }, { "from_samples", an_from_samples }, { "waveform", an_waveform },
    { "window", an_window }, { "fft", an_fft }, { "spectrum", an_spectrum }, { "pitch", an_pitch }, { "onsets", an_onsets }, { "bpm", an_bpm },
};
const int mn_analysis_count = (int)(sizeof mn_analysis_entries / sizeof mn_analysis_entries[0]);
