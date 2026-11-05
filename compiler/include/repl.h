/* KingCobra interactive REPL — `kingcobra repl` (see docs/REPL.md)
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#ifndef KC_REPL_H
#define KC_REPL_H
#include "kc_common.h"

/* True when standard input is an interactive terminal. */
bool kc_stdin_is_tty(void);

/* Run the REPL until EOF / :quit. `preload` (may be NULL) is a .kc file that is
 * executed into the session first (without auto-calling main). `typecheck`
 * is the initial state of the static type checker. Returns a process exit code. */
int kc_repl_main(const char *preload, bool typecheck);

#endif
