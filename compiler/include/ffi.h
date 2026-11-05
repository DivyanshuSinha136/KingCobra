#ifndef KC_FFI_H
#define KC_FFI_H
#include "value.h"
#include "ast.h"

/* KingCobra's native FFI ("extern"). v0.1 scope, stated plainly:
 * supports functions with up to 6 parameters that are ALL int-like
 * or ALL double-like (no mixing), returning int, float, or void.
 * This covers the overwhelming majority of libc / libm / syscall-style
 * calls. Full arbitrary-signature FFI (mixed int/float args, structs
 * by value, varargs) needs a real ABI marshaller (libffi) and is
 * tracked in docs/ROADMAP.md.
 */
KcExternFn *kc_ffi_declare(KcNode *decl, KcPos pos);
KcValue kc_ffi_call(KcExternFn *ext, KcValue *args, int nargs, KcPos pos);

/* Backs native-import calls (`alias.symbol(args)` on a value opened via
 * `import "lib.so" as alias;` — see bind_native_module in interp.c).
 * Unlike kc_ffi_call above, there is no `extern` declaration to read a
 * signature from, so the calling convention is inferred from the
 * ACTUAL argument values at this call: if the first argument is a
 * float, every argument is passed as a double and the result is read
 * as a double; otherwise every argument is passed as an int64 (ints,
 * bools, and strings-as-pointers, exactly like kc_ffi_call) and the
 * result is read as an int64. At most 6 arguments. This is a real but
 * necessarily cruder inference than a declared `extern` gets — use
 * `extern` instead when you need a float-returning function called
 * with integer arguments, or any signature this can't guess right. */
KcValue kc_ffi_call_dynamic(void *fnptr, KcValue *args, int nargs, KcPos pos);

#endif
