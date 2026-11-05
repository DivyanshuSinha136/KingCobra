/* Core standard library: assert/error, higher-order functions, sorting, object helpers,
 * ord/chr/idiv/min/max. Methods are dispatched by a small integer id resolved once per call
 * site (cached in the AST node), so a method call never compares strings. */
#ifndef KC_LIB_H
#define KC_LIB_H
#include "value.h"
#include "interp.h"

typedef enum {
    M_NONE = 0,
    /* strings */
    M_UPPER, M_LOWER, M_TRIM, M_SPLIT, M_CONTAINS, M_INDEX_OF, M_STARTS_WITH, M_ENDS_WITH, M_REPLACE, M_SLICE,
    M_REPEAT, M_PAD_LEFT, M_PAD_RIGHT, M_CHARS, M_LINES,
    /* arrays */
    M_PUSH, M_POP, M_JOIN, M_REVERSE,
    M_MAP, M_FILTER, M_REDUCE, M_FIND, M_ANY, M_ALL, M_EACH, M_SORT, M_CONCAT, M_FIRST, M_LAST,
    M_CLEAR, M_INSERT, M_REMOVE, M_EXTEND, M_SUM, M_MIN, M_MAX,
    /* objects / structs */
    M_KEYS, M_VALUES, M_ITEMS, M_HAS, M_GET, M_MERGE,
    M_COUNT
} KcMethodId;

int kc_method_id(const char *name);
const char *kc_method_hint(const char *name);   /* " (did you mean 'x'?)" for a misspelled built-in method, or "" */   /* M_NONE if the name is not a built-in method */

/* The methods implemented in lib.c. Returns true (and sets *out) if `obj` has built-in method `mid`. */
bool kc_lib_method(KcInterp *ip, KcValue obj, int mid, KcValue *args, int nargs, KcPos pos, KcValue *out);

void kc_register_lib_builtins(KcInterp *ip);
int kc_method_names(int kind, const char **out, int cap);   /* completion: 0 string, 1 array, 2 object */
void kc_register_clib_builtins(KcInterp *ip);
void kc_register_help_builtin(KcInterp *ip);   /* help.c: help([thing]) */   /* clib.c: the C and B standard libraries */
char *kc_format_values(const char *fmt, size_t flen, KcValue *args, int nargs, KcPos pos, const char *fname, size_t *outlen);   /* printf engine; caller frees */
#endif
