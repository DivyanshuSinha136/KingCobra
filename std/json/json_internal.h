/* KingCobra std.json — internal declarations shared by json*.c.
 *
 * Layout (mirrors how std.nn / std.rhex are split):
 *   json.c         parser, serializer, formatter, files, JSON Lines, module aggregation
 *   json_ops.c     object/array helpers, paths & RFC 6901 pointers, equal/copy/merge,
 *                  RFC 6902 patch + diff, RFC 7396 merge-patch, flatten/unflatten
 *   json_query.c   JSONPath-style queries (wildcards, recursive descent, slices, filters)
 *   json_schema.c  JSON-Schema-subset validation
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#ifndef KC_JSON_INTERNAL_H
#define KC_JSON_INTERNAL_H

#include "interp.h"
#include <stdarg.h>

/* ---- growable byte buffer ---- */
typedef struct { char *p; size_t len, cap; } JBuf;
void  jb_init(JBuf *b);
void  jb_putc(JBuf *b, char c);
void  jb_put(JBuf *b, const char *s, size_t n);
void  jb_puts(JBuf *b, const char *s);
void  jb_printf(JBuf *b, const char *fmt, ...);
char *jb_take(JBuf *b, size_t *len);          /* NUL-terminates; caller owns the memory */
KcValue jb_to_string(JBuf *b);                /* NUL-terminates and wraps as a KingCobra string */

/* ---- parsing ---- */
typedef struct {
    bool comments;          /* allow // and block comments */
    bool trailing_commas;   /* allow [1,2,] and {"a":1,} */
    bool nan;               /* allow NaN, Infinity, -Infinity */
    bool dup_error;         /* duplicate object keys are an error (default: last wins) */
    int  max_depth;         /* nesting limit (default 512) */
} JParseOpts;

typedef struct { char msg[160]; int line, col; } JError;

void json_default_parse_opts(JParseOpts *o);
void json_parse_opts_from(KcValue flags, KcPos pos, const char *who, JParseOpts *o);
bool json_parse_text(const char *s, size_t n, const JParseOpts *o, KcValue *out, JError *err);
bool json_reformat_text(const char *s, size_t n, const JParseOpts *o, const char *indent, size_t indlen, JBuf *out, JError *err);

/* ---- serialization ---- */
typedef struct {
    const char *indent; size_t indlen;  /* indent unit; indlen == 0 means compact output */
    bool sort_keys, ascii, nan_null;
} JWriteOpts;

bool json_write_value(JBuf *b, KcValue v, const JWriteOpts *o, char *err, size_t errsz);
bool json_write_string(JBuf *b, const char *s, size_t n, bool ascii);   /* false: input was not valid UTF-8 */
void json_format_double(char *out, double d);
bool json_read_whole_file(const char *path, char **buf, size_t *len);

/* ---- shared value helpers (json_ops.c) ---- */
bool    jv_equal(KcValue a, KcValue b);
KcValue jv_copy(KcValue v);
bool    jv_is_container_obj(KcValue v);       /* object or declared struct */
const char *jv_type_name(KcValue v);          /* null bool integer number string array object, or NULL if not JSON */

/* ---- path tokens (json_ops.c) ---- */
typedef struct { char *s; size_t n; bool bracket; } PTok;
typedef struct { PTok *t; int count; } PPath;
bool  jpath_parse(const char *s, size_t n, PPath *out, char *err, size_t errsz);
KcValue *jpath_resolve(KcValue doc, const PPath *p);         /* pointer to the slot, or NULL */

/* ---- module entry tables, aggregated by json.c ---- */
extern KcModuleEntry json_ops_entries[];    extern const int json_ops_count;
extern KcModuleEntry json_query_entries[];  extern const int json_query_count;
extern KcModuleEntry json_schema_entries[]; extern const int json_schema_count;

/* json_query.c */
bool jq_run(KcInterp *ip, KcPos pos, KcValue doc, const char *expr, KcValue *out_values, KcValue *out_paths, char *err, size_t errsz);
/* json_schema.c */
void js_validate(KcInterp *ip, KcPos pos, KcValue doc, KcValue schema, KcValue root_schema, const char *path, KcValue errors, int depth);

#endif
