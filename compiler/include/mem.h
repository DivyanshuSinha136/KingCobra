/* Explicit memory management: free / free_deep / is_freed / mem_stats / leak report. */
#ifndef KC_MEM_H
#define KC_MEM_H
#include "value.h"
#include "interp.h"
#include <stdio.h>

void kc_error_freed(KcValue v, KcPos pos);                       /* "use after free (this array was freed)" */
void kc_mem_report(FILE *out);
void kc_check_native_args(KcNativeFn f, const KcValue *args, int n, KcPos pos);  /* tombstone check; free/is_freed exempt */
void kc_check_live_args(const KcValue *args, int n, KcPos pos);  /* error if any argument is a tombstone */
void kc_register_mem_builtins(KcInterp *ip);
#endif
