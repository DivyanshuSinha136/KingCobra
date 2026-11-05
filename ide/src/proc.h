/* kcide — child process runner. POSIX: pseudo-terminal (colours, line buffering, input()) or plain pipes.
 * Windows: ConPTY when available, otherwise anonymous pipes. Also: run a program attached to the real terminal. */
#ifndef KCIDE_PROC_H
#define KCIDE_PROC_H
#include "util.h"
typedef struct Proc Proc;
struct Proc {
    bool running; int fd;          /* fd: readable end (also the pty master); -1 on Windows / when closed */
    int wfd;                       /* writable end (== fd for a pty) */
    int pid; int exit_code; bool signaled;
#ifdef _WIN32
    void *hproc, *hin, *hout, *hpc;
#endif
};
/* envkv: NULL-terminated "KEY=VALUE" list added to the child's environment (may be NULL). */
bool proc_start_ex(Proc *p, char *const argv[], const char *cwd, int cols, int rows, const char *const *envkv, bool use_pty);
bool proc_start(Proc *p, char *const argv[], const char *cwd, int cols, int rows);   /* pty, no extra env */
int  proc_read(Proc *p, char *buf, int cap);   /* >0 bytes, 0 nothing yet, -1 stream closed */
void proc_write(Proc *p, const char *d, int n);
bool proc_poll(Proc *p);                       /* reaps the child; returns running */
void proc_stop(Proc *p);                       /* interrupt, then kill */
void proc_resize(Proc *p, int cols, int rows);
void proc_close(Proc *p);
int  proc_run_inherit(char *const argv[], const char *cwd); /* terminal must be suspended by the caller */
#endif
