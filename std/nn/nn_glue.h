/* std.nn script glue: shared helpers for the nn_api_*.c files. Author: Divyanshu Sinha */
#ifndef KC_NN_GLUE_H
#define KC_NN_GLUE_H
/* MinGW's default C runtime lacks %zu/%lld; request the C99-conforming printf family (no effect elsewhere) */
#if defined(__MINGW32__) && !defined(__USE_MINGW_ANSI_STDIO)
#define __USE_MINGW_ANSI_STDIO 1
#endif
#include "interp.h"
#include "nn_internal.h"

extern KcPos *NN_POS;
#define NNAPI() do { NN_POS = &pos; (void)ip; } while (0)
#define NNERR(...) kc_error_at(*NN_POS, __VA_ARGS__)

enum { NNH_TENSOR = 0, NNH_LAYER, NNH_LOSS, NNH_OPT, NNH_COUNT };
KcValue nnh_wrap(int sub, void *p);
void   *nnh_get(KcValue v, int sub);                       /* NULL when v is not a live handle of that kind */
void   *nnh_need(KcValue *a, int n, int i, int sub, const char *fn);
KcValue nn_ret_tensor(NnTensor *t);                        /* t is heap-allocated; ownership moves to the handle */
KcValue nn_to_nested(const NnTensor *t);

typedef struct { NnTensor *t; NnTensor tmp; bool owned; } TArg;
NnTensor *targ_get(TArg *ta, KcValue *a, int n, int i, const char *fn);
void      targ_free(TArg *ta);
bool      nn_value_to_tensor(KcValue v, NnTensor *out, const char *fn);

void    need_n(int n, int lo, int hi, const char *fn);
int64_t arg_int(KcValue *a, int n, int i, const char *fn);
double  arg_num(KcValue *a, int n, int i, const char *fn);
const char *arg_str(KcValue *a, int n, int i, const char *fn);
int     arg_shape(KcValue *a, int n, int i, int *shape, const char *fn);
KcValue mk_struct(const char *type, int cnt, const char **names, KcValue *vals);
KcValue arr_of_doubles(const double *d, int n);
KcValue arr_of_ints(const int64_t *d, int n);

/* options: trailing name/value pairs (or one struct whose fields are the options) */
typedef struct { const char *k; KcValue v; bool used; } NnOpt1;
typedef struct { NnOpt1 o[48]; int n; const char *fn; const char *seen[64]; int nseen; } NnOpts;
void    nnopts_parse(NnOpts *O, KcValue *a, int n, int start, const char *fn);
bool    nnopts_has(NnOpts *O, const char *key);
KcValue *nnopts_val(NnOpts *O, const char *key);
double  nnopts_num(NnOpts *O, const char *key, double def);
int64_t nnopts_int(NnOpts *O, const char *key, int64_t def);
bool    nnopts_bool(NnOpts *O, const char *key, bool def);
const char *nnopts_str(NnOpts *O, const char *key, const char *def);
void    nnopts_done(NnOpts *O);

typedef struct { const char *name; KcNativeFn fn; } NnEntry;
extern const NnEntry nn_tensor_entries[]; extern const int nn_tensor_count;
extern const NnEntry nn_model_entries[];  extern const int nn_model_count;
extern const NnEntry nn_math_entries[];   extern const int nn_math_count;      /* nn_api_math.c   */
extern const NnEntry nn_linalg_entries[]; extern const int nn_linalg_count;    /* nn_api_linalg.c */
KcModule *kc_mod_nn(void);
#endif
