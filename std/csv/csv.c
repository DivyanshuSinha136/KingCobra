/* KingCobra std.csv — RFC 4180 CSV/TSV, streaming and whole-file.
 *
 *  - One parser for memory and files: quoted fields with embedded delimiters, newlines and "" escapes,
 *    CRLF / LF / CR record ends, UTF-8 BOM, comment lines, blank-line handling, delimiter sniffing.
 *  - Header mode returns records as open objects (same type std.json produces), so
 *    json.stringify(csv.parse(text, "header")) just works. Duplicate/empty header names are made unique.
 *  - Optional type inference for unquoted fields (int / float / bool / empty -> null); quoted fields stay strings.
 *  - Strict mode (ragged rows and stray quotes are errors) or lenient mode (the default).
 *  - Writer: correct quoting, records or arrays, column selection, CRLF, BOM, append.
 *  - Streaming reader/writer handles for files that don't fit comfortably in memory.
 *  - Table helpers: column, select, sort_by, filter, transpose, stats, to_records, to_rows.
 *
 * Options are a string of space-separated words ("header infer delim=; trim strict") or an object
 * ({"delimiter": ";", "header": true}). See docs/STD_CSV.md.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "../json/json_internal.h"
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <strings.h>

/* ============================================================ options */

typedef struct {
    char delim, quote;
    bool has_quote, auto_delim;
    bool header, trim, infer, keep_empty, strict, crlf, bom, always_quote, no_header, append;
    char *comment; size_t clen;
    KcValue columns; bool has_columns;
} CsvOpts;

static void opts_default(CsvOpts *o) {
    memset(o, 0, sizeof *o);
    o->delim = ','; o->quote = '"'; o->has_quote = true;
    o->columns = kc_null();
}

static bool parse_delim_word(const char *v, char *out) {
    if (!strcmp(v, "tab") || !strcmp(v, "\\t")) { *out = '\t'; return true; }
    if (!strcmp(v, "comma")) { *out = ','; return true; }
    if (!strcmp(v, "semicolon")) { *out = ';'; return true; }
    if (!strcmp(v, "pipe")) { *out = '|'; return true; }
    if (!strcmp(v, "space")) { *out = ' '; return true; }
    if (strlen(v) == 1) { *out = v[0]; return true; }
    return false;
}

static void opts_apply_string(CsvOpts *o, const char *s, KcPos pos, const char *who) {
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        if (!*s) break;
        const char *w = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n') s++;
        size_t l = (size_t)(s - w);
        char word[96];
        if (l >= sizeof word) kc_error_at(pos, "%s: option too long", who);
        memcpy(word, w, l); word[l] = 0;
        char *eq = strchr(word, '=');
        const char *val = NULL;
        if (eq) { *eq = 0; val = eq + 1; }
        #define IS(x) (!strcmp(word, x))
        if (IS("header")) o->header = true;
        else if (IS("no_header")) o->no_header = true;
        else if (IS("trim")) o->trim = true;
        else if (IS("infer")) o->infer = true;
        else if (IS("keep_empty")) o->keep_empty = true;
        else if (IS("strict")) o->strict = true;
        else if (IS("crlf")) o->crlf = true;
        else if (IS("bom")) o->bom = true;
        else if (IS("always_quote")) o->always_quote = true;
        else if (IS("append")) o->append = true;
        else if (IS("tsv")) o->delim = '\t';
        else if ((IS("delim") || IS("delimiter")) && val) {
            if (!strcmp(val, "auto")) o->auto_delim = true;
            else if (!parse_delim_word(val, &o->delim)) kc_error_at(pos, "%s: delim must be one character or tab/comma/semicolon/pipe/space/auto", who);
        } else if (IS("quote") && val) {
            if (!strcmp(val, "none")) o->has_quote = false;
            else if (strlen(val) == 1) { o->quote = val[0]; o->has_quote = true; }
            else kc_error_at(pos, "%s: quote must be one character or none", who);
        } else if (IS("comment") && val && *val) { o->comment = kc_strdup(val); o->clen = strlen(val); }
        else kc_error_at(pos, "%s: unknown option '%s' (header no_header trim infer keep_empty strict crlf bom always_quote append tsv delim=X quote=X comment=X)", who, word);
        #undef IS
    }
}

static bool truthy_opt(KcValue v) { return v.type == VAL_BOOL ? v.as.b : kc_truthy(v); }

static void opts_from(KcValue v, KcPos pos, const char *who, CsvOpts *o) {
    opts_default(o);
    if (v.type == VAL_NULL) return;
    if (v.type == VAL_STRING) { opts_apply_string(o, v.as.s->data, pos, who); return; }
    if (v.type != VAL_STRUCT) kc_error_at(pos, "%s: options must be a string such as \"header infer\" or an object, got %s", who, kc_type_name(v));
    KcStructInst *s = v.as.strct;
    for (int i = 0; i < s->count; i++) {
        const char *k = s->names[i]; KcValue x = s->values[i];
        if (!strcmp(k, "delimiter") || !strcmp(k, "delim")) {
            if (x.type != VAL_STRING) kc_error_at(pos, "%s: delimiter must be a string", who);
            if (!strcmp(x.as.s->data, "auto")) o->auto_delim = true;
            else if (!parse_delim_word(x.as.s->data, &o->delim)) kc_error_at(pos, "%s: delimiter must be a single character", who);
        } else if (!strcmp(k, "quote")) {
            if (x.type == VAL_STRING && x.as.s->len == 1) { o->quote = x.as.s->data[0]; o->has_quote = true; }
            else if (x.type == VAL_NULL || (x.type == VAL_BOOL && !x.as.b)) o->has_quote = false;
            else kc_error_at(pos, "%s: quote must be a one-character string or null", who);
        } else if (!strcmp(k, "comment")) {
            if (x.type == VAL_STRING && x.as.s->len > 0) { o->comment = kc_strdup(x.as.s->data); o->clen = (size_t)x.as.s->len; }
        } else if (!strcmp(k, "columns")) {
            if (x.type != VAL_ARRAY) kc_error_at(pos, "%s: columns must be an array of names", who);
            o->columns = x; o->has_columns = true;
        }
        else if (!strcmp(k, "header")) o->header = truthy_opt(x);
        else if (!strcmp(k, "no_header")) o->no_header = truthy_opt(x);
        else if (!strcmp(k, "trim")) o->trim = truthy_opt(x);
        else if (!strcmp(k, "infer")) o->infer = truthy_opt(x);
        else if (!strcmp(k, "keep_empty")) o->keep_empty = truthy_opt(x);
        else if (!strcmp(k, "strict")) o->strict = truthy_opt(x);
        else if (!strcmp(k, "crlf")) o->crlf = truthy_opt(x);
        else if (!strcmp(k, "bom")) o->bom = truthy_opt(x);
        else if (!strcmp(k, "always_quote")) o->always_quote = truthy_opt(x);
        else if (!strcmp(k, "append")) o->append = truthy_opt(x);
        else kc_error_at(pos, "%s: unknown option '%s'", who, k);
    }
}

/* ============================================================ byte source */

typedef struct {
    FILE *f;
    const unsigned char *s; size_t n, i;     /* memory source */
    unsigned char buf[65536]; size_t bl, bp; /* file buffer */
    int line;
} Src;

static void src_mem(Src *s, const char *d, size_t n) { memset(s, 0, sizeof *s); s->s = (const unsigned char *)d; s->n = n; s->line = 1; }
static void src_file(Src *s, FILE *f) { memset(s, 0, sizeof *s); s->f = f; s->line = 1; }

static int src_peek(Src *s) {
    if (!s->f) return s->i < s->n ? s->s[s->i] : -1;
    if (s->bp >= s->bl) {
        s->bl = fread(s->buf, 1, sizeof s->buf, s->f);
        s->bp = 0;
        if (s->bl == 0) return -1;
    }
    return s->buf[s->bp];
}
static int src_get(Src *s) {
    int c = src_peek(s);
    if (c < 0) return -1;
    if (s->f) s->bp++; else s->i++;
    if (c == '\n') s->line++;
    return c;
}
static void src_skip_bom(Src *s) {
    if (s->f) {
        if (src_peek(s) == 0xEF && s->bl - s->bp >= 3 && s->buf[s->bp + 1] == 0xBB && s->buf[s->bp + 2] == 0xBF) s->bp += 3;
    } else if (s->n >= 3 && s->s[0] == 0xEF && s->s[1] == 0xBB && s->s[2] == 0xBF) s->i = 3;
}
/* does the upcoming input start with the comment prefix? (needs lookahead within the buffer/memory) */
static bool src_starts_with(Src *s, const char *p, size_t l) {
    if (!s->f) return s->i + l <= s->n && memcmp(s->s + s->i, p, l) == 0;
    if (src_peek(s) < 0) return false;
    if (s->bl - s->bp < l && s->bp > 0) {          /* shift remaining bytes down and refill */
        size_t rem = s->bl - s->bp;
        memmove(s->buf, s->buf + s->bp, rem);
        s->bl = rem + fread(s->buf + rem, 1, sizeof s->buf - rem, s->f);
        s->bp = 0;
    }
    return s->bl - s->bp >= l && memcmp(s->buf + s->bp, p, l) == 0;
}

/* ============================================================ record reader */

typedef struct { char **d; size_t *l; bool *q; int n, cap; } Fields;

static void fields_clear(Fields *f) {
    for (int i = 0; i < f->n; i++) free(f->d[i]);
    f->n = 0;
}
static void fields_add(Fields *f, char *data, size_t len, bool quoted) {
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 16;
        f->d = kc_realloc(f->d, sizeof(char *) * (size_t)f->cap);
        f->l = kc_realloc(f->l, sizeof(size_t) * (size_t)f->cap);
        f->q = kc_realloc(f->q, sizeof(bool) * (size_t)f->cap);
    }
    f->d[f->n] = data; f->l[f->n] = len; f->q[f->n] = quoted;
    f->n++;
}

#define RFAIL(...) do { snprintf(err, errsz, __VA_ARGS__); return -1; } while (0)

/* Returns 1 with a record in *f, 0 at end of input, -1 on error (message in err). */
static int read_record(Src *s, const CsvOpts *o, Fields *f, char *err, size_t errsz) {
    fields_clear(f);
    for (;;) {
        /* skip comment lines and (unless keep_empty) blank lines at record start */
        int c = src_peek(s);
        if (c < 0) return 0;
        if (o->comment && src_starts_with(s, o->comment, o->clen)) {
            while ((c = src_get(s)) >= 0 && c != '\n') {}
            continue;
        }
        if (!o->keep_empty && (c == '\n' || c == '\r')) {
            src_get(s);
            if (c == '\r' && src_peek(s) == '\n') src_get(s);
            continue;
        }
        break;
    }
    int start_line = s->line;
    for (;;) {                                               /* one field per iteration */
        JBuf b; jb_init(&b);
        bool quoted = false;
        int c;
        if (o->trim) while ((c = src_peek(s)) == ' ' || c == '\t') src_get(s);
        c = src_peek(s);
        if (o->has_quote && c == (unsigned char)o->quote) {
            quoted = true;
            src_get(s);
            for (;;) {
                c = src_get(s);
                if (c < 0) { free(b.p); RFAIL("line %d: unterminated quoted field", start_line); }
                if (c == (unsigned char)o->quote) {
                    if (src_peek(s) == (unsigned char)o->quote) { src_get(s); jb_putc(&b, o->quote); continue; }
                    break;
                }
                jb_putc(&b, (char)c);
            }
            if (o->trim) while ((c = src_peek(s)) == ' ' || c == '\t') src_get(s);
            c = src_peek(s);
            if (c >= 0 && c != (unsigned char)o->delim && c != '\n' && c != '\r') {
                if (o->strict) { free(b.p); RFAIL("line %d: unexpected character '%c' after closing quote", s->line, (char)c); }
                while ((c = src_peek(s)) >= 0 && c != (unsigned char)o->delim && c != '\n' && c != '\r') { jb_putc(&b, (char)c); src_get(s); }
            }
        } else {
            while ((c = src_peek(s)) >= 0 && c != (unsigned char)o->delim && c != '\n' && c != '\r') {
                if (o->strict && o->has_quote && c == (unsigned char)o->quote) { free(b.p); RFAIL("line %d: quote character inside an unquoted field", s->line); }
                jb_putc(&b, (char)c); src_get(s);
            }
            if (o->trim) while (b.len > 0 && (b.p[b.len - 1] == ' ' || b.p[b.len - 1] == '\t')) b.len--;
        }
        size_t l; char *d = jb_take(&b, &l);
        fields_add(f, d, l, quoted);
        c = src_peek(s);
        if (c == (unsigned char)o->delim) { src_get(s); continue; }
        if (c == '\r') { src_get(s); if (src_peek(s) == '\n') src_get(s); else s->line++; }
        else if (c == '\n') src_get(s);
        return 1;
    }
}

/* ============================================================ values */

static bool is_int_text(const char *s, size_t n) {
    size_t i = 0;
    if (n && s[0] == '-') i = 1;
    if (i >= n || n - i > 18) return false;
    if (s[i] == '0' && n - i > 1) return false;               /* "007" stays a string (IDs, zip codes) */
    for (; i < n; i++) if (!isdigit((unsigned char)s[i])) return false;
    return true;
}
static bool is_float_text(const char *s, size_t n, double *out) {
    if (!n || n > 63) return false;
    bool digit = false, mark = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (isdigit((unsigned char)c)) digit = true;
        else if (c == '.' || c == 'e' || c == 'E') mark = true;
        else if (c == '-' || c == '+') { /* sign position is validated by strtod */ }
        else return false;
    }
    if (!digit || !mark) return false;
    if (s[0] == '0' && n > 1 && isdigit((unsigned char)s[1])) return false;
    char tmp[64]; memcpy(tmp, s, n); tmp[n] = 0;
    char *end; double d = strtod(tmp, &end);
    if (*end || !isfinite(d)) return false;
    *out = d;
    return true;
}

/* Converts an unquoted field to int / float / bool / null, otherwise leaves it as a string. */
static KcValue infer_value(char *data, size_t len) {
    if (len == 0) { free(data); return kc_null(); }
    if (is_int_text(data, len)) { int64_t v = strtoll(data, NULL, 10); free(data); return kc_int(v); }
    double d;
    if (is_float_text(data, len, &d)) { free(data); return kc_float(d); }
    if (len == 4 && !strcasecmp(data, "true")) { free(data); return kc_bool(true); }
    if (len == 5 && !strcasecmp(data, "false")) { free(data); return kc_bool(false); }
    return kc_string_take(data, (int)len);
}

static KcValue field_value(const CsvOpts *o, Fields *f, int i) {
    char *d = f->d[i]; size_t l = f->l[i];
    f->d[i] = kc_malloc(1); f->d[i][0] = 0;                   /* ownership moves to the value */
    if (o->infer && !f->q[i]) return infer_value(d, l);
    return kc_string_take(d, (int)l);
}

/* make header names unique and non-empty: "", "a", "a" -> "col1", "a", "a_2" */
static KcValue make_header(const CsvOpts *o, Fields *f) {
    KcValue names = kc_array_new();
    for (int i = 0; i < f->n; i++) {
        char base[256];
        if (f->l[i] == 0) snprintf(base, sizeof base, "col%d", i + 1);
        else snprintf(base, sizeof base, "%.*s", (int)(f->l[i] < 250 ? f->l[i] : 250), f->d[i]);
        char name[270]; snprintf(name, sizeof name, "%s", base);
        int suffix = 2;
        for (;;) {
            bool clash = false;
            for (int j = 0; j < names.as.arr->count && !clash; j++) clash = strcmp(names.as.arr->items[j].as.s->data, name) == 0;
            if (!clash) break;
            snprintf(name, sizeof name, "%s_%d", base, suffix++);
        }
        kc_array_push(names, kc_string(name));
    }
    (void)o;
    return names;
}

static KcValue build_row(const CsvOpts *o, Fields *f, KcValue names) {
    if (names.type != VAL_ARRAY) {
        KcValue row = kc_array_new();
        for (int i = 0; i < f->n; i++) kc_array_push(row, field_value(o, f, i));
        return row;
    }
    KcValue rec = kc_object_new();
    int nn = names.as.arr->count;
    for (int i = 0; i < nn; i++) {
        KcString *k = names.as.arr->items[i].as.s;
        kc_object_set(rec, k->data, (size_t)k->len, i < f->n ? field_value(o, f, i) : kc_null());
    }
    for (int i = nn; i < f->n; i++) {                          /* extra fields: col<N> */
        char name[32]; snprintf(name, sizeof name, "col%d", i + 1);
        int suffix = 2;
        while (kc_object_get(rec, name, strlen(name))) snprintf(name, sizeof name, "col%d_%d", i + 1, suffix++);
        kc_object_set(rec, name, strlen(name), field_value(o, f, i));
    }
    return rec;
}

/* ============================================================ sniffing */

static char sniff_delim(const char *s, size_t n) {
    static const char cands[] = { ',', ';', '\t', '|' };
    int counts[4][6]; int lines = 0;
    memset(counts, 0, sizeof counts);
    size_t i = 0;
    if (n >= 3 && (unsigned char)s[0] == 0xEF) i = 3;
    bool inq = false; int cur[4] = {0};
    size_t linelen = 0;
    for (; i < n && lines < 6; i++) {
        char c = s[i];
        if (c == '"') inq = !inq;
        if (!inq && (c == '\n' || c == '\r')) {
            if (c == '\r' && i + 1 < n && s[i + 1] == '\n') i++;
            if (linelen > 0) { for (int k = 0; k < 4; k++) counts[k][lines] = cur[k]; lines++; }
            memset(cur, 0, sizeof cur); linelen = 0;
            continue;
        }
        linelen++;
        if (!inq) for (int k = 0; k < 4; k++) if (c == cands[k]) cur[k]++;
    }
    if (lines == 0 && linelen > 0) { for (int k = 0; k < 4; k++) counts[k][0] = cur[k]; lines = 1; }
    char best = ','; int best_score = 0;
    for (int k = 0; k < 4; k++) {
        bool consistent = lines > 0 && counts[k][0] > 0;
        for (int l = 1; l < lines && consistent; l++) consistent = counts[k][l] == counts[k][0];
        if (consistent && counts[k][0] > best_score) { best_score = counts[k][0]; best = cands[k]; }
    }
    return best;
}

/* ============================================================ helpers */

static void need(int n, int lo, int hi, KcPos pos, const char *sig) {
    if (n < lo || n > hi) kc_error_at(pos, "usage: csv.%s", sig);
}
static KcString *get_str(KcValue v, KcPos pos, const char *who, const char *what) {
    if (v.type != VAL_STRING) kc_error_at(pos, "%s: %s must be a string, got %s", who, what, kc_type_name(v));
    return v.as.s;
}

typedef struct { Src src; Fields f; CsvOpts o; KcValue names; bool names_set; int rows_read; FILE *fp; bool done; } Stream;

/* Reads the next record of a stream as a value. Returns false at end. Fatal on malformed input
   unless err_out is non-NULL (then the message is copied there and false is returned with *failed). */
static bool stream_next(Stream *st, KcValue *out, char *err, size_t errsz, bool *failed) {
    *failed = false;
    if (st->done) return false;
    for (;;) {
        int rc = read_record(&st->src, &st->o, &st->f, err, errsz);
        if (rc < 0) { *failed = true; st->done = true; return false; }
        if (rc == 0) { st->done = true; return false; }
        if (st->o.header && !st->names_set) {
            st->names = make_header(&st->o, &st->f);
            st->names_set = true;
            continue;
        }
        if (st->o.strict && st->names_set && st->f.n != st->names.as.arr->count) {
            snprintf(err, errsz, "line %d has %d field(s), but the header has %d", st->src.line - 1, st->f.n, st->names.as.arr->count);
            *failed = true; st->done = true; return false;
        }
        *out = build_row(&st->o, &st->f, st->names_set ? st->names : kc_null());
        st->rows_read++;
        return true;
    }
}

static void stream_init(Stream *st, const CsvOpts *o) {
    memset(st, 0, sizeof *st);
    st->o = *o;
    st->names = kc_null();
}

static KcValue parse_text(const char *text, size_t n, KcValue optv, KcPos pos, const char *who, bool fatal, bool *ok) {
    CsvOpts o; opts_from(optv, pos, who, &o);
    if (o.auto_delim) o.delim = sniff_delim(text, n);
    Stream *st = kc_malloc(sizeof(Stream));
    stream_init(st, &o);
    src_mem(&st->src, text, n);
    src_skip_bom(&st->src);
    KcValue rows = kc_array_new();
    int width = -1;
    char err[200]; bool failed;
    KcValue row;
    while (stream_next(st, &row, err, sizeof err, &failed)) {
        if (o.strict && !st->names_set) {
            if (width < 0) width = row.as.arr->count;
            else if (row.as.arr->count != width) {
                snprintf(err, sizeof err, "line %d has %d field(s), expected %d", st->src.line - 1, row.as.arr->count, width);
                failed = true; break;
            }
        }
        kc_array_push(rows, row);
    }
    fields_clear(&st->f);
    if (failed) {
        if (ok) *ok = false;
        if (fatal) kc_error_at(pos, "%s: %s", who, err);
        return kc_null();
    }
    if (ok) *ok = true;
    return rows;
}

static bool read_file_all(const char *path, char **buf, size_t *len) { return json_read_whole_file(path, buf, len); }

/* ============================================================ writing */

static bool needs_quote(const char *s, size_t n, const CsvOpts *o, bool is_string) {
    if (n == 0) return is_string;                               /* "" is quoted so it differs from null */
    if (s[0] == ' ' || s[0] == '\t' || s[n - 1] == ' ' || s[n - 1] == '\t') return true;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == o->delim || c == '\n' || c == '\r' || (o->has_quote && c == o->quote)) return true;
    }
    if (o->comment && n >= o->clen && memcmp(s, o->comment, o->clen) == 0) return true;
    return false;
}

static bool put_field(JBuf *b, KcValue v, const CsvOpts *o, char *err, size_t errsz) {
    char num[64];
    const char *text = NULL; size_t tl = 0; bool is_string = false;
    JBuf tmp; jb_init(&tmp);
    bool owned = false;
    switch (v.type) {
        case VAL_NULL: text = ""; tl = 0; break;
        case VAL_BOOL: text = v.as.b ? "true" : "false"; tl = strlen(text); break;
        case VAL_INT: snprintf(num, sizeof num, "%lld", (long long)v.as.i); text = num; tl = strlen(num); break;
        case VAL_FLOAT: json_format_double(num, v.as.f); text = num; tl = strlen(num); break;
        case VAL_STRING: text = v.as.s->data; tl = (size_t)v.as.s->len; is_string = true; break;
        case VAL_ARRAY: case VAL_STRUCT: {                      /* nested values are written as compact JSON */
            JWriteOpts wo = { "", 0, false, false, true };
            if (!json_write_value(&tmp, v, &wo, err, errsz)) { free(tmp.p); return false; }
            jb_putc(&tmp, 0); tmp.len--;
            text = tmp.p; tl = tmp.len; is_string = true; owned = true;
            break;
        }
        default:
            snprintf(err, errsz, "cannot write a value of type '%s' to CSV", kc_type_name(v));
            return false;
    }
    bool q = o->has_quote && (o->always_quote ? v.type != VAL_NULL : needs_quote(text, tl, o, is_string));
    if (!o->has_quote && needs_quote(text, tl, o, false)) {
        snprintf(err, errsz, "a field contains the delimiter or a line break but quoting is disabled");
        if (owned) free(tmp.p);
        return false;
    }
    if (q) {
        jb_putc(b, o->quote);
        for (size_t i = 0; i < tl; i++) { if (text[i] == o->quote) jb_putc(b, o->quote); jb_putc(b, text[i]); }
        jb_putc(b, o->quote);
    } else jb_put(b, text, tl);
    if (owned) free(tmp.p);
    return true;
}

static void put_eol(JBuf *b, const CsvOpts *o) { if (o->crlf) jb_putc(b, '\r'); jb_putc(b, '\n'); }

static bool put_array_record(JBuf *b, KcValue row, const CsvOpts *o, char *err, size_t errsz) {
    for (int i = 0; i < row.as.arr->count; i++) {
        if (i) jb_putc(b, o->delim);
        if (!put_field(b, row.as.arr->items[i], o, err, errsz)) return false;
    }
    put_eol(b, o);
    return true;
}

static bool put_object_record(JBuf *b, KcValue rec, KcValue cols, const CsvOpts *o, char *err, size_t errsz) {
    for (int i = 0; i < cols.as.arr->count; i++) {
        if (i) jb_putc(b, o->delim);
        KcString *k = cols.as.arr->items[i].as.s;
        KcValue *v = kc_object_get(rec, k->data, (size_t)k->len);
        if (!put_field(b, v ? *v : kc_null(), o, err, errsz)) return false;
    }
    put_eol(b, o);
    return true;
}

static KcValue collect_columns(KcValue rows) {
    KcValue cols = kc_array_new();
    for (int i = 0; i < rows.as.arr->count; i++) {
        KcValue r = rows.as.arr->items[i];
        if (r.type != VAL_STRUCT) continue;
        for (int j = 0; j < r.as.strct->count; j++) {
            const char *k = r.as.strct->names[j];
            bool have = false;
            for (int c = 0; c < cols.as.arr->count && !have; c++) have = strcmp(cols.as.arr->items[c].as.s->data, k) == 0;
            if (!have) kc_array_push(cols, kc_string(k));
        }
    }
    return cols;
}

static void check_columns(KcValue cols, KcPos pos, const char *who) {
    for (int i = 0; i < cols.as.arr->count; i++)
        if (cols.as.arr->items[i].type != VAL_STRING) kc_error_at(pos, "%s: column names must be strings", who);
}

static KcValue rows_to_text(KcValue rows, const CsvOpts *o, KcPos pos, const char *who, bool write_header) {
    if (rows.type != VAL_ARRAY) kc_error_at(pos, "%s: expected an array of rows, got %s", who, kc_type_name(rows));
    JBuf b; jb_init(&b);
    char err[200];
    if (o->bom) jb_put(&b, "\xEF\xBB\xBF", 3);
    KcValue cols = o->has_columns ? o->columns : kc_null();
    bool header_done = false;
    for (int i = 0; i < rows.as.arr->count; i++) {
        KcValue r = rows.as.arr->items[i];
        if (r.type == VAL_ARRAY) {
            if (!put_array_record(&b, r, o, err, sizeof err)) { free(b.p); kc_error_at(pos, "%s: row %d: %s", who, i, err); }
        } else if (r.type == VAL_STRUCT) {
            if (cols.type != VAL_ARRAY) { cols = collect_columns(rows); }
            check_columns(cols, pos, who);
            if (!header_done) {
                header_done = true;
                if (write_header && !o->no_header) {
                    if (!put_array_record(&b, cols, o, err, sizeof err)) { free(b.p); kc_error_at(pos, "%s: %s", who, err); }
                }
            }
            if (!put_object_record(&b, r, cols, o, err, sizeof err)) { free(b.p); kc_error_at(pos, "%s: row %d: %s", who, i, err); }
        } else {
            free(b.p);
            kc_error_at(pos, "%s: row %d must be an array or object, got %s", who, i, kc_type_name(r));
        }
    }
    return jb_to_string(&b);
}

/* ============================================================ module functions: whole text/files */

static KcValue c_parse(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "parse(text[, options]) -> rows   (options: \"header infer trim strict delim=; ...\")");
    KcString *s = get_str(a[0], pos, "csv.parse", "text");
    return parse_text(s->data, (size_t)s->len, n > 1 ? a[1] : kc_null(), pos, "csv.parse", true, NULL);
}
static KcValue c_try_parse(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 3, pos, "try_parse(text[, default[, options]]) -> rows or default");
    KcString *s = get_str(a[0], pos, "csv.try_parse", "text");
    bool ok;
    KcValue r = parse_text(s->data, (size_t)s->len, n > 2 ? a[2] : kc_null(), pos, "csv.try_parse", false, &ok);
    return ok ? r : (n > 1 ? a[1] : kc_null());
}
static KcValue c_parse_file(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "parse_file(path[, options]) -> rows");
    KcString *p = get_str(a[0], pos, "csv.parse_file", "path");
    char *buf; size_t len;
    if (!read_file_all(p->data, &buf, &len)) kc_error_at(pos, "csv.parse_file: cannot read '%s': %s", p->data, strerror(errno));
    KcValue r = parse_text(buf, len, n > 1 ? a[1] : kc_null(), pos, "csv.parse_file", true, NULL);
    free(buf);
    return r;
}
static KcValue c_parse_line(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "parse_line(line[, options]) -> array   (one record; options as for parse, header ignored)");
    KcString *s = get_str(a[0], pos, "csv.parse_line", "line");
    CsvOpts o; opts_from(n > 1 ? a[1] : kc_null(), pos, "csv.parse_line", &o);
    o.header = false; o.keep_empty = true;
    Stream *st = kc_malloc(sizeof(Stream));
    stream_init(st, &o);
    src_mem(&st->src, s->data, (size_t)s->len);
    char err[200]; bool failed; KcValue row;
    if (!stream_next(st, &row, err, sizeof err, &failed)) {
        if (failed) kc_error_at(pos, "csv.parse_line: %s", err);
        KcValue e = kc_array_new(); kc_array_push(e, kc_string("")); return e;
    }
    return row;
}
static KcValue c_stringify(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "stringify(rows[, options]) -> string   (rows: arrays or objects; options: delim=X crlf bom always_quote no_header ...)");
    CsvOpts o; opts_from(n > 1 ? a[1] : kc_null(), pos, "csv.stringify", &o);
    return rows_to_text(a[0], &o, pos, "csv.stringify", true);
}
static KcValue c_format_line(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "format_line(array[, options]) -> string   (one record, no trailing newline)");
    if (a[0].type != VAL_ARRAY) kc_error_at(pos, "csv.format_line: expected an array, got %s", kc_type_name(a[0]));
    CsvOpts o; opts_from(n > 1 ? a[1] : kc_null(), pos, "csv.format_line", &o);
    JBuf b; jb_init(&b); char err[200];
    o.crlf = false;
    if (!put_array_record(&b, a[0], &o, err, sizeof err)) { free(b.p); kc_error_at(pos, "csv.format_line: %s", err); }
    b.len--;                                                    /* drop the newline */
    return jb_to_string(&b);
}
static bool write_text_file(const char *path, const char *d, size_t n, const char *mode) {
    FILE *f = fopen(path, mode);
    if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    return ok;
}
static KcValue write_impl(KcInterp *ip, KcValue *a, int n, KcPos pos, const char *who, bool append) {
    kc_forbid_in_comptime(ip, pos, who);
    need(n, 2, 3, pos, append ? "append_file(path, rows[, options]) -> int   (header row is not repeated)"
                              : "write_file(path, rows[, options]) -> int");
    KcString *p = get_str(a[0], pos, who, "path");
    CsvOpts o; opts_from(n > 2 ? a[2] : kc_null(), pos, who, &o);
    if (append) o.bom = false;
    KcValue t = rows_to_text(a[1], &o, pos, who, !append);
    if (!write_text_file(p->data, t.as.s->data, (size_t)t.as.s->len, append ? "ab" : "wb"))
        kc_error_at(pos, "%s: cannot write '%s': %s", who, p->data, strerror(errno));
    return kc_int(a[1].as.arr->count);
}
static KcValue c_write_file(KcInterp *ip, KcValue *a, int n, KcPos pos) { return write_impl(ip, a, n, pos, "csv.write_file", false); }
static KcValue c_append_file(KcInterp *ip, KcValue *a, int n, KcPos pos) { return write_impl(ip, a, n, pos, "csv.append_file", true); }

static KcValue c_sniff(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "sniff(text) -> {delimiter, columns, header}   (best guess from the first lines)");
    KcString *s = get_str(a[0], pos, "csv.sniff", "text");
    char d = sniff_delim(s->data, (size_t)s->len);
    CsvOpts o; opts_default(&o); o.delim = d; o.keep_empty = false;
    Stream *st = kc_malloc(sizeof(Stream));
    stream_init(st, &o);
    src_mem(&st->src, s->data, (size_t)s->len);
    src_skip_bom(&st->src);
    char err[200]; bool failed; KcValue r1 = kc_null(), r2 = kc_null();
    bool have1 = stream_next(st, &r1, err, sizeof err, &failed);
    bool have2 = have1 && stream_next(st, &r2, err, sizeof err, &failed);
    bool header = false;
    if (have1 && have2) {
        bool first_numeric = false, second_numeric = false;
        for (int i = 0; i < r1.as.arr->count; i++) { KcString *x = r1.as.arr->items[i].as.s; double dd; if (is_int_text(x->data, (size_t)x->len) || is_float_text(x->data, (size_t)x->len, &dd)) first_numeric = true; }
        for (int i = 0; i < r2.as.arr->count; i++) { KcString *x = r2.as.arr->items[i].as.s; double dd; if (is_int_text(x->data, (size_t)x->len) || is_float_text(x->data, (size_t)x->len, &dd)) second_numeric = true; }
        header = !first_numeric && second_numeric;
        if (!first_numeric && !second_numeric) {                 /* all text: header if first row has no duplicates-of-data and is short words */
            header = true;
        }
    }
    KcValue res = kc_object_new();
    char ds[2] = { d, 0 };
    kc_object_set(res, "delimiter", 9, kc_string(ds));
    kc_object_set(res, "columns", 7, kc_int(have1 ? r1.as.arr->count : 0));
    kc_object_set(res, "header", 6, kc_bool(header));
    return res;
}

static KcValue c_infer(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "infer(string) -> int | float | bool | null | string   (the same rule the \"infer\" option applies)");
    KcString *s = get_str(a[0], pos, "csv.infer", "value");
    char *d = kc_malloc((size_t)s->len + 1); memcpy(d, s->data, (size_t)s->len); d[s->len] = 0;
    return infer_value(d, (size_t)s->len);
}

/* ============================================================ streaming handles */

static KcValue make_handle(void *ptr, intptr_t extra) {
    KcHandle *h = kc_malloc(sizeof(KcHandle));
    h->kind = H_CSV; h->ptr = ptr; h->size = 0; h->extra = extra;
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h;
    return v;
}

typedef struct { FILE *fp; CsvOpts o; bool header_pending; KcValue cols; bool cols_known; int written; } Writer;

static KcHandle *expect_csv(KcValue v, KcPos pos, const char *who, intptr_t kind) {
    if (v.type != VAL_HANDLE || v.as.handle->kind != H_CSV) kc_error_at(pos, "%s: expected a csv.%s() handle", who, kind ? "writer" : "open");
    if (v.as.handle->extra != kind) kc_error_at(pos, "%s: this is a csv %s handle, not a %s", who, v.as.handle->extra ? "writer" : "reader", kind ? "writer" : "reader");
    if (!v.as.handle->ptr) kc_error_at(pos, "%s: the handle is already closed", who);
    return v.as.handle;
}

static KcValue c_open(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "open(path[, options]) -> reader   (stream records with csv.read; close with csv.close)");
    KcString *p = get_str(a[0], pos, "csv.open", "path");
    CsvOpts o; opts_from(n > 1 ? a[1] : kc_null(), pos, "csv.open", &o);
    FILE *fp = fopen(p->data, "rb");
    if (!fp) kc_error_at(pos, "csv.open: cannot open '%s': %s", p->data, strerror(errno));
    if (o.auto_delim) {                                           /* sniff from the first 8 KB, then rewind */
        char samp[8192]; size_t got = fread(samp, 1, sizeof samp, fp);
        o.delim = sniff_delim(samp, got);
        rewind(fp);
    }
    Stream *st = kc_malloc(sizeof(Stream));
    stream_init(st, &o);
    st->fp = fp;
    src_file(&st->src, fp);
    src_skip_bom(&st->src);
    if (o.header) {                                               /* read the header eagerly so csv.header works */
        char err[200];
        if (read_record(&st->src, &st->o, &st->f, err, sizeof err) < 0) { fclose(fp); kc_error_at(pos, "csv.open: %s", err); }
        if (st->f.n > 0) { st->names = make_header(&st->o, &st->f); st->names_set = true; }
    }
    return make_handle(st, 0);
}
static KcValue c_read(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "read(reader) -> row or null at end");
    Stream *st = expect_csv(a[0], pos, "csv.read", 0)->ptr;
    char err[200]; bool failed; KcValue row;
    if (stream_next(st, &row, err, sizeof err, &failed)) return row;
    if (failed) kc_error_at(pos, "csv.read: %s", err);
    return kc_null();
}
static KcValue c_header(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "header(reader) -> string[] or null");
    Stream *st = expect_csv(a[0], pos, "csv.header", 0)->ptr;
    return st->names_set ? st->names : kc_null();
}
static KcValue c_line(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "line(reader) -> int   (line number the reader has consumed up to)");
    Stream *st = expect_csv(a[0], pos, "csv.line", 0)->ptr;
    return kc_int(st->src.line);
}

static KcValue c_writer(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "csv.writer");
    need(n, 1, 2, pos, "writer(path[, options]) -> writer   (options: append, columns, delim=X, crlf, bom, no_header ...)");
    KcString *p = get_str(a[0], pos, "csv.writer", "path");
    Writer *w = kc_malloc(sizeof(Writer));
    memset(w, 0, sizeof *w);
    opts_from(n > 1 ? a[1] : kc_null(), pos, "csv.writer", &w->o);
    w->fp = fopen(p->data, w->o.append ? "ab" : "wb");
    if (!w->fp) kc_error_at(pos, "csv.writer: cannot open '%s': %s", p->data, strerror(errno));
    if (w->o.bom && !w->o.append) fwrite("\xEF\xBB\xBF", 1, 3, w->fp);
    w->cols = w->o.columns; w->cols_known = w->o.has_columns;
    w->header_pending = !w->o.append && !w->o.no_header;
    return make_handle(w, 1);
}
static void writer_row(Writer *w, KcValue row, KcPos pos, const char *who) {
    JBuf b; jb_init(&b); char err[200];
    if (row.type == VAL_STRUCT) {
        if (!w->cols_known) {
            w->cols = kc_array_new();
            for (int i = 0; i < row.as.strct->count; i++) kc_array_push(w->cols, kc_string(row.as.strct->names[i]));
            w->cols_known = true;
        }
        check_columns(w->cols, pos, who);
        if (w->header_pending) { if (!put_array_record(&b, w->cols, &w->o, err, sizeof err)) { free(b.p); kc_error_at(pos, "%s: %s", who, err); } w->header_pending = false; }
        if (!put_object_record(&b, row, w->cols, &w->o, err, sizeof err)) { free(b.p); kc_error_at(pos, "%s: %s", who, err); }
    } else if (row.type == VAL_ARRAY) {
        w->header_pending = false;
        if (!put_array_record(&b, row, &w->o, err, sizeof err)) { free(b.p); kc_error_at(pos, "%s: %s", who, err); }
    } else kc_error_at(pos, "%s: a row must be an array or object, got %s", who, kc_type_name(row));
    size_t l; char *d = jb_take(&b, &l);
    if (fwrite(d, 1, l, w->fp) != l) { free(d); kc_error_at(pos, "%s: write failed: %s", who, strerror(errno)); }
    free(d);
    w->written++;
}
static KcValue c_write(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "csv.write");
    need(n, 2, 2, pos, "write(writer, row) -> null   (row: array or object)");
    Writer *w = expect_csv(a[0], pos, "csv.write", 1)->ptr;
    writer_row(w, a[1], pos, "csv.write");
    return kc_null();
}
static KcValue c_write_rows(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "csv.write_rows");
    need(n, 2, 2, pos, "write_rows(writer, rows) -> int");
    Writer *w = expect_csv(a[0], pos, "csv.write_rows", 1)->ptr;
    if (a[1].type != VAL_ARRAY) kc_error_at(pos, "csv.write_rows: rows must be an array");
    for (int i = 0; i < a[1].as.arr->count; i++) writer_row(w, a[1].as.arr->items[i], pos, "csv.write_rows");
    return kc_int(a[1].as.arr->count);
}
static KcValue c_flush(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "flush(writer) -> null");
    Writer *w = expect_csv(a[0], pos, "csv.flush", 1)->ptr;
    fflush(w->fp);
    return kc_null();
}
static KcValue c_close(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "close(handle) -> null   (reader or writer)");
    if (a[0].type != VAL_HANDLE || a[0].as.handle->kind != H_CSV) kc_error_at(pos, "csv.close: expected a csv reader/writer handle");
    KcHandle *h = a[0].as.handle;
    if (!h->ptr) return kc_null();
    if (h->extra == 1) { Writer *w = h->ptr; fclose(w->fp); }
    else { Stream *st = h->ptr; if (st->fp) fclose(st->fp); fields_clear(&st->f); }
    h->ptr = NULL;
    return kc_null();
}
static KcValue c_count_rows(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2 - 1, 2, pos, "count_rows(path[, options]) -> int   (streams the file; the header row is not counted)");
    KcString *p = get_str(a[0], pos, "csv.count_rows", "path");
    CsvOpts o; opts_from(n > 1 ? a[1] : kc_null(), pos, "csv.count_rows", &o);
    FILE *fp = fopen(p->data, "rb");
    if (!fp) kc_error_at(pos, "csv.count_rows: cannot open '%s': %s", p->data, strerror(errno));
    Src *s = kc_malloc(sizeof(Src));
    src_file(s, fp); src_skip_bom(s);
    Fields f; memset(&f, 0, sizeof f);
    char err[200]; int64_t count = 0; int rc;
    while ((rc = read_record(s, &o, &f, err, sizeof err)) > 0) count++;
    fields_clear(&f); fclose(fp);
    if (rc < 0) kc_error_at(pos, "csv.count_rows: %s", err);
    if (o.header && count > 0) count--;
    return kc_int(count);
}

/* ============================================================ table helpers */

static KcValue cell_of(KcValue row, KcValue col) {
    if (row.type == VAL_STRUCT && col.type == VAL_STRING) {
        KcValue *v = kc_object_get(row, col.as.s->data, (size_t)col.as.s->len);
        return v ? *v : kc_null();
    }
    if (row.type == VAL_ARRAY && col.type == VAL_INT) {
        int64_t i = col.as.i; if (i < 0) i += row.as.arr->count;
        return (i >= 0 && i < row.as.arr->count) ? row.as.arr->items[i] : kc_null();
    }
    return kc_null();
}

/* For arrays-of-arrays with a column NAME, the first row is the header: resolve to an index. */
static KcValue resolve_col(KcValue rows, KcValue col, KcPos pos, const char *who, int *first_data_row) {
    *first_data_row = 0;
    if (rows.type != VAL_ARRAY) kc_error_at(pos, "%s: rows must be an array", who);
    if (col.type == VAL_INT) return col;
    if (col.type != VAL_STRING) kc_error_at(pos, "%s: a column is a name (string) or index (int)", who);
    if (rows.as.arr->count == 0) return col;
    KcValue r0 = rows.as.arr->items[0];
    if (r0.type == VAL_STRUCT) return col;
    if (r0.type == VAL_ARRAY) {
        for (int i = 0; i < r0.as.arr->count; i++) {
            KcValue h = r0.as.arr->items[i];
            if (h.type == VAL_STRING && h.as.s->len == col.as.s->len && memcmp(h.as.s->data, col.as.s->data, (size_t)h.as.s->len) == 0) { *first_data_row = 1; return kc_int(i); }
        }
        kc_error_at(pos, "%s: no column named '%s' in the header row", who, col.as.s->data);
    }
    kc_error_at(pos, "%s: rows must be arrays or objects", who);
    return col;
}

static KcValue t_column(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "column(rows, name_or_index) -> array   (for arrays-of-arrays, a name is looked up in the first row)");
    int start; KcValue col = resolve_col(a[0], a[1], pos, "csv.column", &start);
    KcValue out = kc_array_new();
    for (int i = start; i < a[0].as.arr->count; i++) kc_array_push(out, cell_of(a[0].as.arr->items[i], col));
    return out;
}

static KcValue t_select(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "select(rows, columns) -> rows   (keep only the listed columns, in that order)");
    if (a[0].type != VAL_ARRAY || a[1].type != VAL_ARRAY) kc_error_at(pos, "csv.select: expected (rows array, columns array)");
    KcValue out = kc_array_new();
    KcArray *rows = a[0].as.arr;
    if (rows->count > 0 && rows->items[0].type == VAL_ARRAY) {
        int nc = a[1].as.arr->count;
        int *idx = kc_malloc(sizeof(int) * (size_t)(nc ? nc : 1));
        for (int c = 0; c < nc; c++) {
            int start; KcValue ci = resolve_col(a[0], a[1].as.arr->items[c], pos, "csv.select", &start);
            idx[c] = (int)ci.as.i;
        }
        for (int i = 0; i < rows->count; i++) {
            KcValue nr = kc_array_new();
            for (int c = 0; c < nc; c++) kc_array_push(nr, cell_of(rows->items[i], kc_int(idx[c])));
            kc_array_push(out, nr);
        }
        free(idx);
        return out;
    }
    for (int i = 0; i < rows->count; i++) {
        if (rows->items[i].type != VAL_STRUCT) kc_error_at(pos, "csv.select: row %d is not an object", i);
        KcValue rec = kc_object_new();
        for (int c = 0; c < a[1].as.arr->count; c++) {
            KcValue k = a[1].as.arr->items[c];
            if (k.type != VAL_STRING) kc_error_at(pos, "csv.select: column names must be strings for records");
            KcValue *v = kc_object_get(rows->items[i], k.as.s->data, (size_t)k.as.s->len);
            kc_object_set(rec, k.as.s->data, (size_t)k.as.s->len, v ? *v : kc_null());
        }
        kc_array_push(out, rec);
    }
    return out;
}

static bool numeric_of(KcValue v, double *out) {
    if (v.type == VAL_INT) { *out = (double)v.as.i; return true; }
    if (v.type == VAL_FLOAT) { *out = v.as.f; return isfinite(v.as.f); }
    if (v.type == VAL_STRING) {
        const char *s = v.as.s->data; size_t l = (size_t)v.as.s->len;
        if (!l || l > 63) return false;
        char *end; double d = strtod(s, &end);
        if (end != s + l || !isfinite(d) || isspace((unsigned char)s[0])) return false;
        *out = d; return true;
    }
    return false;
}

static KcValue g_sort_col; static bool g_sort_desc; static KcValue *g_sort_rows;
typedef struct { int idx; KcValue key; } SortItem;
static int sort_cmp(const void *x, const void *y) {
    const SortItem *a = x, *b = y;
    bool an = a->key.type == VAL_NULL, bn = b->key.type == VAL_NULL;
    if (an || bn) { if (an && bn) return a->idx - b->idx; return an ? 1 : -1; }   /* nulls always last */
    double da, db; int c;
    if (numeric_of(a->key, &da) && numeric_of(b->key, &db)) c = da < db ? -1 : da > db ? 1 : 0;
    else {
        char *sa = kc_to_display_string(a->key), *sb = kc_to_display_string(b->key);
        c = strcmp(sa, sb); free(sa); free(sb);
    }
    if (g_sort_desc) c = -c;
    return c ? c : a->idx - b->idx;
}
static KcValue t_sort_by(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 3, pos, "sort_by(rows, column[, \"desc\"]) -> rows   (stable; numbers compare numerically, nulls last)");
    int start; KcValue col = resolve_col(a[0], a[1], pos, "csv.sort_by", &start);
    bool desc = false;
    if (n > 2) {
        if (a[2].type != VAL_STRING || (strcmp(a[2].as.s->data, "desc") && strcmp(a[2].as.s->data, "asc"))) kc_error_at(pos, "csv.sort_by: order must be \"asc\" or \"desc\"");
        desc = !strcmp(a[2].as.s->data, "desc");
    }
    int cnt = a[0].as.arr->count - start;
    SortItem *items = kc_malloc(sizeof(SortItem) * (size_t)(cnt > 0 ? cnt : 1));
    for (int i = 0; i < cnt; i++) { items[i].idx = i; items[i].key = cell_of(a[0].as.arr->items[start + i], col); }
    g_sort_desc = desc; g_sort_col = col; g_sort_rows = NULL;
    qsort(items, (size_t)cnt, sizeof(SortItem), sort_cmp);
    KcValue out = kc_array_new();
    if (start == 1) kc_array_push(out, a[0].as.arr->items[0]);
    for (int i = 0; i < cnt; i++) kc_array_push(out, a[0].as.arr->items[start + items[i].idx]);
    free(items);
    return out;
}

static KcValue t_filter(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "filter(rows, fn) -> rows   (keeps rows for which fn(row) is truthy; a header row of an array table is kept)");
    if (a[0].type != VAL_ARRAY) kc_error_at(pos, "csv.filter: rows must be an array");
    if (a[1].type != VAL_CLOSURE && a[1].type != VAL_NATIVE) kc_error_at(pos, "csv.filter: second argument must be a function");
    KcValue out = kc_array_new();
    bool arrays = a[0].as.arr->count > 0 && a[0].as.arr->items[0].type == VAL_ARRAY;
    (void)arrays;
    for (int i = 0; i < a[0].as.arr->count; i++) {
        KcValue row = a[0].as.arr->items[i];
        KcValue r = kc_call_value(ip, a[1], &row, 1, pos);
        if (kc_truthy(r)) kc_array_push(out, row);
    }
    return out;
}

static KcValue t_transpose(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "transpose(rows) -> rows   (arrays of arrays; short rows are padded with null)");
    if (a[0].type != VAL_ARRAY) kc_error_at(pos, "csv.transpose: rows must be an array");
    int w = 0;
    for (int i = 0; i < a[0].as.arr->count; i++) {
        KcValue r = a[0].as.arr->items[i];
        if (r.type != VAL_ARRAY) kc_error_at(pos, "csv.transpose: row %d is not an array", i);
        if (r.as.arr->count > w) w = r.as.arr->count;
    }
    KcValue out = kc_array_new();
    for (int c = 0; c < w; c++) {
        KcValue nr = kc_array_new();
        for (int i = 0; i < a[0].as.arr->count; i++) kc_array_push(nr, cell_of(a[0].as.arr->items[i], kc_int(c)));
        kc_array_push(out, nr);
    }
    return out;
}

static KcValue t_stats(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "stats(rows, column) -> {count, sum, mean, min, max, nulls, non_numeric}   (numeric strings count as numbers)");
    int start; KcValue col = resolve_col(a[0], a[1], pos, "csv.stats", &start);
    int64_t cnt = 0, nulls = 0, bad = 0; double sum = 0, mn = 0, mx = 0;
    for (int i = start; i < a[0].as.arr->count; i++) {
        KcValue v = cell_of(a[0].as.arr->items[i], col);
        double d;
        if (v.type == VAL_NULL || (v.type == VAL_STRING && v.as.s->len == 0)) { nulls++; continue; }
        if (!numeric_of(v, &d)) { bad++; continue; }
        if (cnt == 0 || d < mn) mn = d;
        if (cnt == 0 || d > mx) mx = d;
        sum += d; cnt++;
    }
    KcValue r = kc_object_new();
    kc_object_set(r, "count", 5, kc_int(cnt));
    kc_object_set(r, "sum", 3, cnt ? kc_float(sum) : kc_int(0));
    kc_object_set(r, "mean", 4, cnt ? kc_float(sum / (double)cnt) : kc_null());
    kc_object_set(r, "min", 3, cnt ? kc_float(mn) : kc_null());
    kc_object_set(r, "max", 3, cnt ? kc_float(mx) : kc_null());
    kc_object_set(r, "nulls", 5, kc_int(nulls));
    kc_object_set(r, "non_numeric", 11, kc_int(bad));
    return r;
}

static KcValue t_to_records(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "to_records(rows) -> objects   (first row is the header)");
    if (a[0].type != VAL_ARRAY || a[0].as.arr->count == 0 || a[0].as.arr->items[0].type != VAL_ARRAY) kc_error_at(pos, "csv.to_records: expected an array of arrays with a header row");
    KcArray *rows = a[0].as.arr;
    Fields hf; memset(&hf, 0, sizeof hf);
    for (int i = 0; i < rows->items[0].as.arr->count; i++) {
        char *s = kc_to_display_string(rows->items[0].as.arr->items[i]);
        fields_add(&hf, s, strlen(s), true);
    }
    CsvOpts o; opts_default(&o);
    KcValue names = make_header(&o, &hf);
    KcValue out = kc_array_new();
    for (int i = 1; i < rows->count; i++) {
        KcValue r = rows->items[i];
        if (r.type != VAL_ARRAY) kc_error_at(pos, "csv.to_records: row %d is not an array", i);
        KcValue rec = kc_object_new();
        for (int c = 0; c < names.as.arr->count; c++) {
            KcString *k = names.as.arr->items[c].as.s;
            kc_object_set(rec, k->data, (size_t)k->len, cell_of(r, kc_int(c)));
        }
        kc_array_push(out, rec);
    }
    return out;
}

static KcValue t_to_rows(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "to_rows(records[, columns]) -> rows   (header row first; columns default to the union of keys)");
    if (a[0].type != VAL_ARRAY) kc_error_at(pos, "csv.to_rows: records must be an array of objects");
    KcValue cols = n > 1 ? a[1] : collect_columns(a[0]);
    if (cols.type != VAL_ARRAY) kc_error_at(pos, "csv.to_rows: columns must be an array of names");
    check_columns(cols, pos, "csv.to_rows");
    KcValue out = kc_array_new();
    kc_array_push(out, cols);
    for (int i = 0; i < a[0].as.arr->count; i++) {
        if (a[0].as.arr->items[i].type != VAL_STRUCT) kc_error_at(pos, "csv.to_rows: item %d is not an object", i);
        KcValue r = kc_array_new();
        for (int c = 0; c < cols.as.arr->count; c++) kc_array_push(r, cell_of(a[0].as.arr->items[i], cols.as.arr->items[c]));
        kc_array_push(out, r);
    }
    return out;
}

/* ============================================================ registration */

static KcModuleEntry ENTRIES[] = {
    {"parse", c_parse}, {"try_parse", c_try_parse}, {"parse_file", c_parse_file}, {"parse_line", c_parse_line},
    {"stringify", c_stringify}, {"format_line", c_format_line},
    {"write_file", c_write_file}, {"append_file", c_append_file},
    {"sniff", c_sniff}, {"infer", c_infer}, {"count_rows", c_count_rows},
    {"open", c_open}, {"read", c_read}, {"header", c_header}, {"line", c_line},
    {"writer", c_writer}, {"write", c_write}, {"write_rows", c_write_rows}, {"flush", c_flush}, {"close", c_close},
    {"column", t_column}, {"select", t_select}, {"sort_by", t_sort_by}, {"filter", t_filter},
    {"transpose", t_transpose}, {"stats", t_stats}, {"to_records", t_to_records}, {"to_rows", t_to_rows},
};

KcModule *kc_mod_csv(void) {
    static KcModule m = { "csv", ENTRIES, sizeof(ENTRIES) / sizeof(ENTRIES[0]) };
    return &m;
}
