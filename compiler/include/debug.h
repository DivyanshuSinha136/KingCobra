/* Source-level debugger support (`kingcobra debug FILE`).
 *
 * The interpreter calls kc_dbg_stmt() before every statement and kc_dbg_enter() on every call
 * while kc_dbg_on is set. An IDE drives the session over two named pipes (FIFOs on POSIX, named
 * pipes on Windows) whose base path comes from $KC_DEBUG_PIPE: `<base>.cmd` (IDE -> debuggee) and
 * `<base>.evt` (debuggee -> IDE). Messages are single lines of TAB-separated fields. See docs/DEBUGGER.md. */
#ifndef KC_DEBUG_H
#define KC_DEBUG_H
#include "interp.h"

extern int kc_dbg_on;
bool kc_dbg_init(KcInterp *ip, const char *pipe_base, char *err, size_t errcap);   /* connect, handshake, wait for `go` */
void kc_dbg_stmt(KcInterp *ip, KcEnv *env, KcNode *n);
void kc_dbg_enter(KcInterp *ip, KcEnv *fenv, KcClosure *cl, KcPos callpos);
void kc_dbg_finish(void);                                                          /* tell the IDE the program ended */
#endif
