/* KingCobra std.nn — internal interfaces shared by the nn_*.c modules.
 *
 *   nn_core.c    tensors, RNG, weight initializers, GEMM kernels
 *   nn_tensor.c  tensor math for scripts: broadcasting, reductions, matmul, permute, concat
 *   nn_math.c    advanced tensor math: special functions, reductions, scans, sorting, shape ops, einsum, stats
 *   nn_linalg.c  dense linear algebra: LU, Cholesky, QR, symmetric eigen (Jacobi), SVD (one-sided Jacobi)
 *   nn_layers.c  dense, activations, dropout, flatten/reshape, embedding, batch/layer norm
 *   nn_conv.c    conv1d/conv2d, pooling
 *   nn_rnn.c     RNN / LSTM / GRU with backprop through time
 *   nn_attn.c    multi-head self-attention, sinusoidal positional encoding
 *   nn_model.c   layer registry, containers (sequential/residual), params, summary, gradcheck
 *   nn_loss.c    loss functions
 *   nn_optim.c   optimizers, LR schedules, gradient clipping
 *   nn_train.c   fit / evaluate / predict, metrics
 *   nn_io.c      binary save/load with CRC32
 *   nn.c, nn_api_*.c   the script-facing module
 *
 * This layer knows nothing about the interpreter: errors go through nn_fail().
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#ifndef KC_NN_INTERNAL_H
#define KC_NN_INTERNAL_H
/* MinGW's default C runtime lacks %zu/%lld; request the C99-conforming printf family (no effect elsewhere) */
#if defined(__MINGW32__) && !defined(__USE_MINGW_ANSI_STDIO)
#define __USE_MINGW_ANSI_STDIO 1
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* compact one-line `if (..) a; if (..) b;` forms are used deliberately throughout std.nn */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif

typedef double nn_real;
#define NN_MAXDIM 6

/* raises a script error and never returns (implemented by the glue layer) */
#if defined(__MINGW32__)
#define NN_PRINTF_ARCH gnu_printf     /* with __USE_MINGW_ANSI_STDIO the C99 implementation is what runs */
#else
#define NN_PRINTF_ARCH printf
#endif
void nn_fail(const char *fmt, ...) __attribute__((noreturn, format(NN_PRINTF_ARCH, 1, 2)));

/* ------------------------------------------------------------------ memory */
void *nn_malloc(size_t n);
void *nn_calloc(size_t c, size_t s);
void *nn_realloc(void *p, size_t n);
char *nn_strdup(const char *s);

/* ------------------------------------------------------------------ tensor */
typedef struct { int nd; int shape[NN_MAXDIM]; size_t n, cap; nn_real *d; } NnTensor;

size_t nt_numel(int nd, const int *shape);
void   nt_init(NnTensor *t);
void   nt_resize(NnTensor *t, int nd, const int *shape);   /* contents undefined */
void   nt_alloc(NnTensor *t, int nd, const int *shape);    /* zero-filled */
void   nt_free(NnTensor *t);
void   nt_copy(NnTensor *dst, const NnTensor *src);
void   nt_zero(NnTensor *t);
bool   nt_same_shape(const NnTensor *a, const NnTensor *b);
const char *nt_shape_str(int nd, const int *shape);        /* static buffer, 4 rotating slots */
NnTensor *nt_new(int nd, const int *shape);                /* heap tensor, zero-filled */
void   nt_delete(NnTensor *t);

/* ------------------------------------------------------------------ rng */
typedef struct { uint64_t s[4]; double spare; int has_spare; } NnRngState;
void   nn_seed(uint64_t seed);
double nn_rand(void);               /* [0,1) */
double nn_randn(void);              /* N(0,1) */
int64_t nn_randint(int64_t n);      /* [0,n) */
void   nn_rng_save(NnRngState *st);
void   nn_rng_restore(const NnRngState *st);

/* ------------------------------------------------------------------ init */
enum { NNI_NONE = 0, NNI_XAVIER_U, NNI_XAVIER_N, NNI_HE_N, NNI_HE_U, NNI_LECUN_N, NNI_LECUN_U,
       NNI_NORMAL, NNI_UNIFORM, NNI_ZEROS, NNI_ONES, NNI_CONST, NNI_ORTHOGONAL, NNI_COUNT };
int  nn_init_from_name(const char *name);                  /* -1 if unknown */
void nn_init_tensor(NnTensor *w, int fan_in, int fan_out, int init, double arg);

/* ------------------------------------------------------------------ gemm (accumulating: C += ...) */
void nn_gemm_nn(int M, int N, int K, const nn_real *A, const nn_real *B, nn_real *C);  /* A[M,K] B[K,N]   */
void nn_gemm_nt(int M, int N, int K, const nn_real *A, const nn_real *B, nn_real *C);  /* A[M,K] B[N,K]^T */
void nn_gemm_tn(int M, int N, int K, const nn_real *A, const nn_real *B, nn_real *C);  /* A[K,M]^T B[K,N] */

/* ------------------------------------------------------------------ script-level tensor math */
enum { NTB_ADD, NTB_SUB, NTB_MUL, NTB_DIV, NTB_POW, NTB_MAX, NTB_MIN,
       NTB_ATAN2, NTB_HYPOT, NTB_MOD, NTB_FLOORDIV, NTB_EQ, NTB_NE, NTB_LT, NTB_LE, NTB_GT, NTB_GE,
       NTB_AND, NTB_OR, NTB_XOR, NTB_LOGADDEXP };
enum { NTU_NEG, NTU_ABS, NTU_EXP, NTU_LOG, NTU_SQRT, NTU_SQUARE, NTU_TANH, NTU_SIGMOID, NTU_RELU, NTU_SIGN,
       NTU_FLOOR, NTU_CEIL, NTU_ROUND, NTU_SIN, NTU_COS };
enum { NTR_SUM, NTR_MEAN, NTR_MAX, NTR_MIN, NTR_VAR, NTR_STD, NTR_ARGMAX, NTR_ARGMIN, NTR_PROD };
void nt_binop(int op, const NnTensor *a, const NnTensor *b, NnTensor *out);
void nt_unary(int op, const NnTensor *a, NnTensor *out);
#define NT_AXIS_ALL 1000
void nt_reduce(int op, const NnTensor *a, int axis /* NT_AXIS_ALL = everything */, bool keepdims, NnTensor *out);
void nt_matmul(const NnTensor *a, const NnTensor *b, NnTensor *out);
void nt_permute(const NnTensor *a, const int *axes, NnTensor *out);
void nt_concat(const NnTensor *const *ts, int n, int axis, NnTensor *out);
void nt_rows(const NnTensor *a, const int64_t *idx, int64_t n, NnTensor *out);      /* gather along axis 0 */
void nt_softmax_axis(const NnTensor *a, int axis, NnTensor *out);
int  nt_norm_axis(int axis, int nd, const char *fn);

/* ------------------------------------------------------------------ layers */
typedef struct {
    char name[24];
    NnTensor w, g;
    bool is_buffer;    /* running statistics: saved, never optimized, no gradient */
    bool decay;        /* weight decay applies (weights yes; biases/norm params no) */
    bool frozen;
    long skip_lo, skip_hi; /* elements [lo,hi) are held fixed by design (embedding padding row): gradcheck ignores them */
} NnParam;

typedef struct NnLayer NnLayer;
typedef struct {
    void (*forward)(NnLayer *L, const NnTensor *x, int training);
    void (*backward)(NnLayer *L, const NnTensor *dy);          /* fills L->dx when L->need_dx */
    void (*destroy)(NnLayer *L);
} NnOps;

enum { NNL_DENSE = 1, NNL_ACT, NNL_DROPOUT, NNL_FLATTEN, NNL_RESHAPE, NNL_EMBED, NNL_BATCHNORM, NNL_LAYERNORM,
       NNL_CONV, NNL_POOL, NNL_GAP, NNL_RNN, NNL_ATTN, NNL_POSENC, NNL_SEQ, NNL_RESIDUAL, NNL_KIND_MAX };

struct NnLayer {
    int kind;
    char name[32];
    double hp[16]; int nhp;               /* numeric hyper-parameters; layout documented per kind */
    const NnOps *ops;
    NnParam *params; int nparams;
    NnLayer **kids; int nkids, kidcap;    /* containers */
    NnTensor out, dx;
    const NnTensor *in;                   /* borrowed between forward and backward */
    bool need_dx;
    bool training;
    bool has_forward;
    bool has_parent;                      /* already owned by a container */
    void *priv;
};

NnLayer *nn_layer_new(int kind, const double *hp, int nhp, int init, double init_arg);
void     nn_layer_free(NnLayer *L);
void     nn_forward(NnLayer *L, const NnTensor *x, int training);
void     nn_backward(NnLayer *L, const NnTensor *dy);
NnParam *nn_param_add(NnLayer *L, const char *name, int nd, const int *shape, bool decay, bool buffer);
void     nn_container_append(NnLayer *C, NnLayer *child);
const char *nn_kind_name(int kind);
int      nn_collect_params(NnLayer *L, NnParam ***out);     /* recursive; caller frees *out */
long     nn_count_params(NnLayer *L, bool trainable_only);
void     nn_zero_grad(NnLayer *L);
void     nn_set_frozen(NnLayer *L, bool frozen);
NnLayer *nn_clone(const NnLayer *L);

/* per-kind setup (called by nn_layer_new) and ops tables */
void nn_setup_dense(NnLayer *L, int init, double arg);     extern const NnOps nn_ops_dense;
void nn_setup_act(NnLayer *L, int init, double arg);       extern const NnOps nn_ops_act;
void nn_setup_dropout(NnLayer *L, int init, double arg);   extern const NnOps nn_ops_dropout;
void nn_setup_flatten(NnLayer *L, int init, double arg);   extern const NnOps nn_ops_flatten;
void nn_setup_reshape(NnLayer *L, int init, double arg);   extern const NnOps nn_ops_reshape;
void nn_setup_embed(NnLayer *L, int init, double arg);     extern const NnOps nn_ops_embed;
void nn_setup_batchnorm(NnLayer *L, int init, double arg); extern const NnOps nn_ops_batchnorm;
void nn_setup_layernorm(NnLayer *L, int init, double arg); extern const NnOps nn_ops_layernorm;
void nn_setup_conv(NnLayer *L, int init, double arg);      extern const NnOps nn_ops_conv;
void nn_setup_pool(NnLayer *L, int init, double arg);      extern const NnOps nn_ops_pool;
void nn_setup_gap(NnLayer *L, int init, double arg);       extern const NnOps nn_ops_gap;
void nn_setup_rnn(NnLayer *L, int init, double arg);       extern const NnOps nn_ops_rnn;
void nn_setup_attn(NnLayer *L, int init, double arg);      extern const NnOps nn_ops_attn;
void nn_setup_posenc(NnLayer *L, int init, double arg);    extern const NnOps nn_ops_posenc;
extern const NnOps nn_ops_seq, nn_ops_residual;

/* activation ids (hp[0] of NNL_ACT) */
enum { NNA_RELU, NNA_RELU6, NNA_LEAKY, NNA_ELU, NNA_SELU, NNA_GELU, NNA_SIGMOID, NNA_TANH, NNA_SOFTPLUS,
       NNA_SILU, NNA_MISH, NNA_SOFTSIGN, NNA_HARDSIG, NNA_SOFTMAX, NNA_LOGSOFTMAX, NNA_IDENTITY, NNA_COUNT };
int  nn_act_from_name(const char *name);                   /* -1 if unknown */
const char *nn_act_name(int id);

/* shape helper shared by layers: fail with a readable message */
void nn_shape_fail(const NnLayer *L, const char *expect, const NnTensor *x) __attribute__((noreturn));

/* ------------------------------------------------------------------ losses */
enum { NNLS_MSE, NNLS_MAE, NNLS_HUBER, NNLS_LOGCOSH, NNLS_BCE, NNLS_BCE_LOGITS, NNLS_CE, NNLS_NLL, NNLS_KL, NNLS_HINGE, NNLS_COUNT };
typedef struct { int kind; double delta, smoothing, pos_weight; int64_t ignore_index; bool has_ignore; NnTensor grad; } NnLoss;
int      nn_loss_from_name(const char *name);
const char *nn_loss_name(int id);
NnLoss  *nn_loss_new(int kind);
void     nn_loss_free(NnLoss *l);
double   nn_loss_eval(NnLoss *l, const NnTensor *pred, const NnTensor *target, bool want_grad);

/* ------------------------------------------------------------------ optimizers */
enum { NNO_SGD, NNO_ADAM, NNO_ADAMW, NNO_RMSPROP, NNO_ADAGRAD, NNO_COUNT };
typedef struct {
    int kind; double lr, base_lr, beta1, beta2, eps, momentum, wd, alpha;
    bool nesterov, amsgrad, centered, decay_all;
    NnLayer *model; NnParam **params; int np; long step;
    NnTensor *s1, *s2, *s3;               /* per-param state */
} NnOpt;
int      nn_opt_from_name(const char *name);
NnOpt   *nn_opt_new(int kind, NnLayer *model);
void     nn_opt_free(NnOpt *o);
void     nn_opt_step(NnOpt *o);
double   nn_clip_grad_norm(NnLayer *m, double max_norm);   /* returns the pre-clip norm */
void     nn_clip_grad_value(NnLayer *m, double v);
/* LR schedule spec, e.g. "cosine:1e-5", "step:10:0.5", "exp:0.95", "linear:0", "const" */
bool     nn_sched_valid(const char *spec, char *err, size_t errn);
double   nn_sched_lr(const char *spec, double base, int epoch, int total, int warmup);

/* ------------------------------------------------------------------ training */
enum { NNM_NONE, NNM_ACC, NNM_MAE, NNM_MSE, NNM_RMSE, NNM_R2, NNM_TOPK };
typedef struct { int kind, k; double a, b, c, d; long n; } NnMetric;
bool   nn_metric_parse(const char *name, int *kind, int *k);
void   nn_metric_update(NnMetric *m, const NnTensor *pred, const NnTensor *target);
double nn_metric_value(const NnMetric *m);
const char *nn_metric_name(int kind, int k);

typedef struct {
    int epochs, batch_size, verbose, patience, warmup, accum, val_every;
    bool shuffle, restore_best, drop_last;
    double clip_norm, clip_value, min_delta, val_split;
    const char *schedule;
    int metric, metric_k;
    int (*callback)(void *ud, int epoch, double loss, double vloss, double metric);
    void *ud;
    const NnTensor *vx, *vy;
} NnFitCfg;
typedef struct {
    int epochs_run, best_epoch; bool stopped_early; double best_val, seconds;
    double *loss, *val_loss, *metric, *val_metric, *lr; int cap;
} NnHistory;
void   nn_fit(NnLayer *m, NnLoss *loss, NnOpt *opt, const NnTensor *X, const NnTensor *Y, const NnFitCfg *cfg, NnHistory *h);
void   nn_history_free(NnHistory *h);
double nn_train_step(NnLayer *m, NnLoss *loss, NnOpt *opt, const NnTensor *x, const NnTensor *y);
double nn_evaluate(NnLayer *m, NnLoss *loss, const NnTensor *X, const NnTensor *Y, int batch, int metric, int metric_k, double *metric_out);
void   nn_predict(NnLayer *m, const NnTensor *X, int batch, NnTensor *out);
void   nn_backward_loss(NnLayer *m, NnLoss *loss, bool input_grad, NnTensor *dx_out);
bool   nn_gradcheck(NnLayer *m, NnLoss *loss, const NnTensor *x, const NnTensor *y, double eps, int samples,
                    double *max_rel, long *checked, char *worst, size_t worstn);
char  *nn_summary(NnLayer *m, int nd, const int *input_shape);   /* malloc'd */

/* ------------------------------------------------------------------ serialization */
typedef struct { unsigned char *p; size_t len, cap; } NnBuf;
void   nnbuf_init(NnBuf *b);
void   nnbuf_free(NnBuf *b);
void   nnbuf_put(NnBuf *b, const void *p, size_t n);
unsigned char *nn_serialize(const NnLayer *m, size_t *len);          /* malloc'd, includes header + CRC */
NnLayer *nn_deserialize(const unsigned char *p, size_t len, char *err, size_t errn);
bool   nn_save_file(const NnLayer *m, const char *path, char *err, size_t errn);
NnLayer *nn_load_file(const char *path, char *err, size_t errn);
bool   nn_save_weights(const NnLayer *m, const char *path, char *err, size_t errn);
bool   nn_load_weights(NnLayer *m, const char *path, char *err, size_t errn);
void   nn_copy_weights(NnLayer *dst, const NnLayer *src, double tau);


/* ------------------------------------------------------------------ advanced math (nn_math.c) */
void nn_act_forward_elems(int id, double a, size_t n, const nn_real *x, nn_real *y);   /* elementwise activations (not softmax) */

enum { NMU_ERF, NMU_ERFC, NMU_ERFINV, NMU_LGAMMA, NMU_DIGAMMA, NMU_LOG1P, NMU_EXPM1, NMU_TAN, NMU_ASIN, NMU_ACOS, NMU_ATAN,
       NMU_SINH, NMU_COSH, NMU_ASINH, NMU_ACOSH, NMU_ATANH, NMU_RSQRT, NMU_RECIP, NMU_CBRT, NMU_LOG2, NMU_LOG10, NMU_EXP2,
       NMU_TRUNC, NMU_FRAC, NMU_LOGSIGMOID, NMU_NDTR, NMU_NDTRI, NMU_LOGIT, NMU_ISNAN, NMU_ISINF, NMU_ISFINITE, NMU_LNOT, NMU_COUNT };
void nm_unary(int op, const NnTensor *a, NnTensor *out);
double nm_digamma(double x);
double nm_erfinv(double y);
double nm_ndtri(double p);
void nm_act(int id, double alpha, bool deriv, const NnTensor *x, NnTensor *out);   /* activation or its derivative d act/dx */

void nm_where(const NnTensor *c, const NnTensor *a, const NnTensor *b, NnTensor *out);

enum { NMR_ANY, NMR_ALL, NMR_COUNT, NMR_LOGSUMEXP, NMR_NORM, NMR_QUANTILE, NMR_NANSUM, NMR_NANMEAN, NMR_PTP };
void nm_reduce(int op, const NnTensor *a, int axis, bool keepdims, double param, NnTensor *out);
enum { NMC_SUM, NMC_PROD, NMC_MAX, NMC_MIN, NMC_LOGSUMEXP };
void nm_cum(int op, const NnTensor *a, int axis, bool reverse, NnTensor *out);
void nm_diff(const NnTensor *a, int order, int axis, NnTensor *out);
void nm_sort(const NnTensor *a, int axis, bool descending, NnTensor *vals, NnTensor *idx);   /* vals / idx may be NULL */
void nm_topk(const NnTensor *a, int k, int axis, bool largest, NnTensor *vals, NnTensor *idx);

void nm_broadcast_to(const NnTensor *a, int nd, const int *shape, NnTensor *out);
void nm_flip(const NnTensor *a, const bool *axes, NnTensor *out);
void nm_slice_axis(const NnTensor *a, int axis, int64_t start, int64_t stop, int64_t step, NnTensor *out);
void nm_tile(const NnTensor *a, const int *reps, int nr, NnTensor *out);
void nm_repeat(const NnTensor *a, int64_t times, int axis, NnTensor *out);
void nm_roll(const NnTensor *a, int64_t shift, int axis, NnTensor *out);
enum { NMP_CONST, NMP_EDGE, NMP_REFLECT, NMP_WRAP };
void nm_pad(const NnTensor *a, const int *before, const int *after, int mode, double value, NnTensor *out);
void nm_take(const NnTensor *a, const int64_t *idx, int64_t n, int axis, NnTensor *out);
void nm_gather(const NnTensor *a, const NnTensor *idx, int axis, NnTensor *out);
void nm_scatter(const NnTensor *base, const NnTensor *idx, const NnTensor *src, int axis, bool add, NnTensor *out);
void nm_tri(const NnTensor *a, int64_t k, bool upper, NnTensor *out);
void nm_diag(const NnTensor *a, int64_t k, NnTensor *out);

void nm_outer(const NnTensor *a, const NnTensor *b, NnTensor *out);
void nm_kron(const NnTensor *a, const NnTensor *b, NnTensor *out);
void nm_tensordot(const NnTensor *a, const NnTensor *b, const int *ax_a, const int *ax_b, int nax, NnTensor *out);
void nm_bmatmul(const NnTensor *a, const NnTensor *b, NnTensor *out);                 /* [..,M,K] x [..,K,N] with batch broadcasting */
void nm_einsum(const char *spec, const NnTensor *const *ts, int nt, NnTensor *out);
void nm_cross(const NnTensor *a, const NnTensor *b, NnTensor *out);

void nm_cov(const NnTensor *x, int ddof, NnTensor *out);                                /* x [N,D] observations in rows -> [D,D] */
void nm_corrcoef(const NnTensor *x, NnTensor *out);
void nm_cdist(const NnTensor *a, const NnTensor *b, double p, NnTensor *out);
void nm_histogram(const NnTensor *x, int bins, double lo, double hi, NnTensor *out);
void nm_bincount(const NnTensor *x, int minlength, NnTensor *out);
enum { NMV_FULL, NMV_SAME, NMV_VALID };
void nm_convolve(const NnTensor *a, const NnTensor *v, int mode, NnTensor *out);

/* ------------------------------------------------------------------ dense linear algebra (nn_linalg.c); row-major n x n unless noted */
int    nl_lu(double *A, int n, int *piv);                                 /* in place; returns +1/-1 (permutation parity) or 0 when singular */
void   nl_lu_solve(const double *LU, const int *piv, int n, double *B, int k);   /* B[n,k] overwritten with the solution */
double nl_det(const double *A, int n);
void   nl_slogdet(const double *A, int n, double *sign, double *logabs);
bool   nl_inv(const double *A, int n, double *out);                       /* false when singular */
bool   nl_solve(const double *A, int n, const double *B, int k, double *X);
bool   nl_cholesky(const double *A, int n, double *L);                    /* lower factor; false when not positive definite */
void   nl_chol_solve(const double *L, int n, double *B, int k);
void   nl_qr(const double *A, int m, int n, double *Q, double *R);        /* reduced: Q[m,k] R[k,n], k = min(m,n) */
void   nl_eigh(const double *A, int n, double *w, double *V);             /* uses the lower triangle; w ascending, V columns are eigenvectors */
void   nl_svd(const double *A, int m, int n, double *U, double *S, double *Vt);   /* reduced: U[m,k] S[k] Vt[k,n], S descending */
void   nl_matmul(int M, int N, int K, const double *A, const double *B, double *C);   /* C = A B (overwrites) */
void   nl_expm(const double *A, int n, double *out);

#endif
