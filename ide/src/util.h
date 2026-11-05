/* kcide — shared helpers: memory, strings, UTF-8, paths, fuzzy matching, directory listing.
 * Author: Divyanshu Sinha. Pure C11 (POSIX or Win32 behind #ifdef). */
#ifndef KCIDE_UTIL_H
#define KCIDE_UTIL_H
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xfmt(const char *fmt, ...);

/* growable NUL-terminated byte string */
typedef struct { char *p; size_t n, cap; } Str;
void str_init(Str *s);
void str_free(Str *s);
void str_clear(Str *s);
void str_add(Str *s, const char *d, size_t n);
void str_adds(Str *s, const char *d);
void str_addc(Str *s, char c);
void str_addf(Str *s, const char *fmt, ...);
char *str_take(Str *s); /* returns malloc'd buffer, resets s */

/* UTF-8 */
int utf8_decode(const char *s, size_t n, uint32_t *cp); /* bytes consumed (>=1); bad bytes -> U+FFFD */
int utf8_encode(uint32_t cp, char *out);                /* bytes written */
int utf8_prev(const char *s, int i);                    /* start offset of the char before offset i */
int cp_width(uint32_t cp);                              /* display columns: 0, 1 or 2 */
bool cp_is_word(uint32_t cp);                           /* identifier character */
bool cp_is_space(uint32_t cp);
uint32_t cp_lower(uint32_t cp);

/* fuzzy subsequence score: -1 when pat is not a subsequence of s, higher is better */
int fuzzy_score(const char *pat, const char *s);

/* paths (results are malloc'd unless noted) */
char *path_join(const char *a, const char *b);
char *path_dirname(const char *p);
const char *path_basename(const char *p); /* pointer into p */
const char *path_ext(const char *p);      /* ".kc" or "" */
char *path_abs(const char *p);
bool path_is_abs(const char *p);
bool is_dir(const char *p);
bool is_file(const char *p);
char *path_rel(const char *root, const char *p); /* p relative to root when under it, else copy of p */
bool path_eq(const char *a, const char *b);
char *home_config_dir(void); /* ~/.config/kcide or %APPDATA%\kcide (created if possible) */
bool make_dir(const char *p);

typedef struct { char *name; bool dir; } DirEnt;
typedef struct { DirEnt *v; int n, cap; } DirList;
bool dir_list(const char *path, DirList *out); /* sorted: dirs first, then names (case-insens.) */
void dir_list_free(DirList *d);

char *read_file(const char *path, size_t *len);
bool write_file_atomic(const char *path, const char *data, size_t len);
long file_mtime(const char *path);
uint64_t now_ms(void);
bool find_in_path(const char *exe, char *out, size_t cap);
#endif
