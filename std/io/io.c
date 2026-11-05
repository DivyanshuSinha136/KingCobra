/* KingCobra std.io — file and console I/O.
 *
 * Deliberately built on plain ANSI C <stdio.h> rather than any
 * platform-specific API: fopen/fread/fwrite/fclose/remove/fgets work
 * identically on Linux, macOS, and Windows, so unlike std.network or
 * std.rhex, this module needs no kc_platform.h branching at all.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "interp.h"

static KcValue io_read_file(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "io.read_file(path: string) -> string");
    FILE *f = fopen(a[0].as.s->data, "rb");
    if (!f) kc_error_at(pos, "io.read_file: cannot open '%s'", a[0].as.s->data);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); kc_error_at(pos, "io.read_file: cannot determine size of '%s'", a[0].as.s->data); }
    fseek(f, 0, SEEK_SET);
    char *buf = kc_malloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = 0;
    fclose(f);
    return kc_string_adopt(buf, (int)got);
}

static KcValue io_write_impl(KcInterp *ip, KcValue *a, int n, KcPos pos, const char *mode, const char *who) {
    kc_forbid_in_comptime(ip, pos, who);
    if (n != 2 || a[0].type != VAL_STRING || a[1].type != VAL_STRING)
        kc_error_at(pos, "%s(path: string, content: string) -> int", who);
    FILE *f = fopen(a[0].as.s->data, mode);
    if (!f) kc_error_at(pos, "%s: cannot open '%s' for writing", who, a[0].as.s->data);
    size_t written = fwrite(a[1].as.s->data, 1, (size_t)a[1].as.s->len, f);
    fclose(f);
    return kc_int((int64_t)written);
}

static KcValue io_write_file(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    return io_write_impl(ip, a, n, pos, "wb", "io.write_file");
}

static KcValue io_append_file(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    return io_write_impl(ip, a, n, pos, "ab", "io.append_file");
}

static KcValue io_exists(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "io.exists(path: string) -> bool");
    FILE *f = fopen(a[0].as.s->data, "rb");
    if (!f) return kc_bool(false);
    fclose(f);
    return kc_bool(true);
}

static KcValue io_delete(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "io.delete");
    if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "io.delete(path: string) -> bool");
    return kc_bool(remove(a[0].as.s->data) == 0);
}

/* io.read_line() reads one line from stdin, trailing newline stripped.
 * Returns an empty string at end-of-input (doesn't distinguish from a
 * genuinely blank line in v0.2 — a that/null-vs-empty distinction is a
 * small roadmap item if it turns out to matter in practice). */
static KcValue io_read_line(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a;
    kc_forbid_in_comptime(ip, pos, "io.read_line");
    if (n != 0) kc_error_at(pos, "io.read_line() -> string");
    char buf[65536];
    if (!fgets(buf, sizeof buf, stdin)) return kc_string("");
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
    return kc_string(buf);
}

/* io.lines(path) -> string[] : whole-file read, split on '\n' (with any
 * trailing '\r' trimmed per line so it also reads CRLF files cleanly). */
static KcValue io_lines(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    KcValue whole = io_read_file(ip, a, n, pos);
    KcValue arr = kc_array_new();
    const char *s = whole.as.s->data;
    const char *line_start = s;
    for (const char *p = s; ; p++) {
        if (*p == '\n' || *p == '\0') {
            size_t len = (size_t)(p - line_start);
            if (len > 0 && line_start[len - 1] == '\r') len--;
            char *line = kc_malloc(len + 1);
            memcpy(line, line_start, len);
            line[len] = 0;
            /* skip a trailing empty line caused by a final newline */
            if (!(*p == '\0' && len == 0 && arr.as.arr->count > 0))
                kc_array_push(arr, kc_string_adopt(line, (int)len));
            else
                free(line);
            line_start = p + 1;
            if (*p == '\0') break;
        }
    }
    return arr;
}

static KcModuleEntry ENTRIES[] = {
    {"read_file", io_read_file}, {"write_file", io_write_file}, {"append_file", io_append_file},
    {"exists", io_exists}, {"delete", io_delete}, {"read_line", io_read_line}, {"lines", io_lines},
};

KcModule *kc_mod_io(void) {
    static KcModule m = { "io", ENTRIES, sizeof(ENTRIES)/sizeof(ENTRIES[0]) };
    return &m;
}
