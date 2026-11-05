#ifndef KC_METHODS_H
#define KC_METHODS_H
#include "value.h"

/* Built-in method-call syntax for strings and arrays: `s.split(",")`,
 * `arr.push(x)`, etc. Tried before falling back to ordinary member
 * access (module functions, struct fields that happen to hold a
 * closure) — see the N_CALL case in compiler/src/interp.c.
 *
 * Returns true and sets *out if `name` is a recognized method for
 * obj's type; returns false (leaving *out untouched) otherwise, so the
 * caller can fall back to normal member lookup. */
bool kc_try_builtin_method_id(KcInterp *ip, KcValue obj, const char *name, int mid, KcValue *args, int nargs, KcPos pos, KcValue *out);
bool kc_try_builtin_method(KcInterp *ip, KcValue obj, const char *name, KcValue *args, int nargs, KcPos pos, KcValue *out);

#endif
