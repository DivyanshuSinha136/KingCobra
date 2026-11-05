/* KingCobra std.system — shared glue for the system_*.c files.
 *
 * std.system is split the same way std.nn and std.rhex are: one file per
 * concern, each exposing a small `SysEntry` table that system.c (the
 * module's "nn.c equivalent") concatenates into the single KcModule the
 * interpreter binds `import std.system;` to.
 *
 *   system.c        module table, env vars, script args, last_error, legacy v0.1 entries
 *   system_info.c   OS / CPU / memory / user / terminal / disk information
 *   system_path.c   path manipulation + directory & file-metadata operations
 *   system_time.c   wall clock, monotonic clock, sleep, date formatting, random bytes
 *   system_proc.c   running commands: shell, argv (no shell), spawn/wait/kill/pipes
 *
 * Error model (matches std.mmio / std.rhex — there is no try/except yet):
 *   - programming errors (wrong argument count/type, use of a freed process
 *     handle, bad option) are fatal with a positioned message;
 *   - environmental failures (file not found, permission denied, command not
 *     found, unsupported on this OS) make the call return null — or false for
 *     boolean operations — and set system.last_error() to a readable string.
 *
 * comptime: pure reads (platform/os/arch, env lookups, cwd, pid, path string
 * functions, metadata reads) are allowed inside `comptime { }`; anything that
 * mutates the outside world, spawns processes, or reads a *volatile* value
 * (clock, free memory, randomness, uptime) is refused via kc_forbid_in_comptime.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#ifndef KC_SYSTEM_GLUE_H
#define KC_SYSTEM_GLUE_H
#include "interp.h"
#include "kc_platform.h"
#include <stdarg.h>
#include <errno.h>

typedef struct { const char *name; KcNativeFn fn; } SysEntry;
extern const SysEntry sys_core_entries[];  extern const int sys_core_count;
extern const SysEntry sys_info_entries[];  extern const int sys_info_count;
extern const SysEntry sys_path_entries[];  extern const int sys_path_count;
extern const SysEntry sys_time_entries[];  extern const int sys_time_count;
extern const SysEntry sys_proc_entries[];  extern const int sys_proc_count;

/* ---- environmental-failure channel (system.last_error()) ---- */
void        sys_set_error(const char *fmt, ...);
void        sys_set_errno(const char *what);          /* "<what>: <strerror(errno)>" (GetLastError text on Windows) */
const char *sys_get_error(void);

/* ---- argument helpers: fatal, positioned usage error on misuse ---- */
void        sys_usage(KcPos pos, const char *sig);                          /* never returns */
void        sys_argc(int n, int lo, int hi, const char *sig, KcPos pos);
const char *sys_str(KcValue *a, int i, const char *sig, KcPos pos);         /* required string  */
int64_t     sys_int(KcValue *a, int i, const char *sig, KcPos pos);         /* required int     */
double      sys_num(KcValue *a, int i, const char *sig, KcPos pos);         /* int or float     */
bool        sys_opt_bool(KcValue *a, int n, int i, bool def, const char *sig, KcPos pos);
double      sys_opt_num(KcValue *a, int n, int i, double def, const char *sig, KcPos pos);
bool        sys_is_nullish(KcValue *a, int n, int i);                       /* absent or null   */

/* ---- value builders ---- */
KcValue     sys_str_val(const char *s);
KcValue     sys_bytes_str(char *owned, size_t len);                          /* takes ownership; binary-safe */
KcValue     sys_struct(const char *type, int n, const char *const *names, const KcValue *vals);

/* Refuse inside comptime { } with the standard message. */
#define SYS_IMPURE(ip, pos, name) kc_forbid_in_comptime((ip), (pos), (name))

/* Script arguments (set once from main.c). */
void        kc_system_set_args(int argc, char **argv);

/* Growable byte buffer shared by the process pump and path helpers. */
typedef struct { char *p; size_t len, cap; } SysBuf;
void        sysbuf_append(SysBuf *b, const char *d, size_t n);
void        sysbuf_free(SysBuf *b);

#endif
