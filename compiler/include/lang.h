/* Language-level runtime helpers (v0.7): `in`, slicing, error objects, enum/impl support. */
#ifndef KC_LANG_H
#define KC_LANG_H
#include "value.h"
#include "interp.h"
#include "env.h"

bool kc_value_in(KcInterp *ip, KcValue x, KcValue container, KcPos pos);                 /* x in container */
KcValue kc_slice_value(KcInterp *ip, KcValue obj, KcValue lo, KcValue hi, KcPos pos);     /* obj[lo..hi]; null = open end */

KcValue kc_error_object(const char *message, KcPos pos, const char *kind);   /* {message, kind, file, line, col} */
char *kc_thrown_text(KcValue v);   /* message of an error object, else the display text; caller frees */

/* "did you mean": bounded Levenshtein over the names in scope / the fields of a struct.
 * Returns a candidate or NULL. The text is a static buffer ready to append to an error message:
 * " (did you mean 'x'?)" or "". */
const char *kc_hint_for_name(KcEnv *env, const char *name);
const char *kc_hint_for_field(KcStructInst *st, const char *name);
const char *kc_hint_for_module(const char **names, int n, const char *name);

/* impl-block methods */
void kc_method_add(KcInterp *ip, const char *type, const char *name, KcValue fn, bool instance);
bool kc_method_find(KcInterp *ip, const char *type, const char *name, KcValue *fn, bool *instance);
const char *kc_value_type_key(KcValue v);   /* the type name `impl` blocks attach to */

#endif
