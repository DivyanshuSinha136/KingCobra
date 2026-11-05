#ifndef KC_PREPROC_H
#define KC_PREPROC_H
#include "kc_common.h"
typedef struct KcPP KcPP;
KcPP *kc_pp_new(void);
const char *kc_preprocess(const char *src, const char *filename, KcPP *pp);   /* returns src itself when nothing to do */
void kc_pp_define(KcPP *pp, const char *name, const char *body);
bool kc_pp_undefine(KcPP *pp, const char *name);
bool kc_pp_is_defined(KcPP *pp, const char *name);
int kc_pp_macro_count(KcPP *pp);
const char *kc_pp_macro_name(KcPP *pp, int i);
const char *kc_pp_macro_body(KcPP *pp, int i);
bool kc_pp_macro_is_func(KcPP *pp, int i);
void kc_pp_macro_params(KcPP *pp, int i, char *out, size_t cap);
void kc_pp_add_cmdline_define(const char *s);   /* -DNAME or -DNAME=value */
#endif
