/* KingCobra std.json — JSON Schema validation (practical subset of drafts 7 / 2020-12).
 *
 *  type (incl. "integer"), enum, const, $ref ("#", "#/definitions/x", "#/$defs/x", any "#/..." pointer),
 *  minimum maximum exclusiveMinimum exclusiveMaximum multipleOf,
 *  minLength maxLength pattern format (email ipv4 uuid date date-time uri),
 *  items (schema or tuple) prefixItems additionalItems minItems maxItems uniqueItems contains,
 *  properties required additionalProperties patternProperties minProperties maxProperties,
 *  allOf anyOf oneOf not, if/then/else.
 * Unknown keywords are ignored, as the spec requires. `validate` returns every violation found,
 * each as "<json pointer>: <message>".
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "json_internal.h"
#include <ctype.h>
#include <math.h>

static void add_err(KcValue errors, const char *path, const char *fmt, ...) {
    char msg[300];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    JBuf b; jb_init(&b);
    jb_puts(&b, (path && *path) ? path : "(root)");
    jb_puts(&b, ": ");
    jb_puts(&b, msg);
    kc_array_push(errors, jb_to_string(&b));
}

static KcValue *sk(KcValue schema, const char *key) { return kc_object_get(schema, key, strlen(key)); }

static bool is_num(KcValue v) { return v.type == VAL_INT || v.type == VAL_FLOAT; }
static double num_of(KcValue v) { return v.type == VAL_INT ? (double)v.as.i : v.as.f; }

static bool type_matches(KcValue d, const char *t) {
    if (!strcmp(t, "null")) return d.type == VAL_NULL;
    if (!strcmp(t, "boolean")) return d.type == VAL_BOOL;
    if (!strcmp(t, "integer")) return d.type == VAL_INT || (d.type == VAL_FLOAT && isfinite(d.as.f) && floor(d.as.f) == d.as.f);
    if (!strcmp(t, "number")) return is_num(d);
    if (!strcmp(t, "string")) return d.type == VAL_STRING;
    if (!strcmp(t, "array")) return d.type == VAL_ARRAY;
    if (!strcmp(t, "object")) return d.type == VAL_STRUCT;
    return false;
}

static const char *schema_type_name(KcValue d) {
    switch (d.type) {
        case VAL_NULL: return "null"; case VAL_BOOL: return "boolean";
        case VAL_INT: return "integer"; case VAL_FLOAT: return "number";
        case VAL_STRING: return "string"; case VAL_ARRAY: return "array";
        case VAL_STRUCT: return "object"; default: return "unknown";
    }
}

static int utf8_count(const char *s, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

static bool regx_test(KcInterp *ip, KcPos pos, const char *pat, KcValue text) {
    KcModule *rm = kc_mod_regx();
    for (int i = 0; i < rm->count; i++) {
        if (strcmp(rm->entries[i].name, "test") == 0) {
            KcValue args[2] = { kc_string(pat), text };
            KcValue r = rm->entries[i].fn(ip, args, 2, pos);
            return r.type == VAL_BOOL && r.as.b;
        }
    }
    return false;
}

/* ---- formats ---- */
static bool fmt_digits(const char *s, int n) { for (int i = 0; i < n; i++) if (!isdigit((unsigned char)s[i])) return false; return true; }
static bool fmt_date(const char *s, size_t n) {
    if (n != 10 || s[4] != '-' || s[7] != '-' || !fmt_digits(s, 4) || !fmt_digits(s + 5, 2) || !fmt_digits(s + 8, 2)) return false;
    int y = atoi(s), m = atoi(s + 5), d = atoi(s + 8);
    static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12 || d < 1) return false;
    int max = dim[m - 1] + ((m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) ? 1 : 0);
    return d <= max;
}
static bool fmt_datetime(const char *s, size_t n) {
    if (n < 20 || !fmt_date(s, 10) || (s[10] != 'T' && s[10] != 't' && s[10] != ' ')) return false;
    const char *t = s + 11;
    if (!(fmt_digits(t, 2) && t[2] == ':' && fmt_digits(t + 3, 2) && t[5] == ':' && fmt_digits(t + 6, 2))) return false;
    if (atoi(t) > 23 || atoi(t + 3) > 59 || atoi(t + 6) > 60) return false;
    size_t i = 19;
    if (i < n && s[i] == '.') { i++; size_t st = i; while (i < n && isdigit((unsigned char)s[i])) i++; if (i == st) return false; }
    if (i < n && (s[i] == 'Z' || s[i] == 'z')) return i + 1 == n;
    if (i + 6 == n && (s[i] == '+' || s[i] == '-') && fmt_digits(s + i + 1, 2) && s[i + 3] == ':' && fmt_digits(s + i + 4, 2)) return true;
    return false;
}
static bool fmt_ipv4(const char *s, size_t n) {
    int parts = 0; size_t i = 0;
    while (i < n) {
        size_t st = i; int v = 0;
        while (i < n && isdigit((unsigned char)s[i]) && i - st < 4) { v = v * 10 + (s[i] - '0'); i++; }
        if (i == st || i - st > 3 || v > 255 || (i - st > 1 && s[st] == '0')) return false;
        parts++;
        if (i < n) { if (s[i] != '.') return false; i++; if (i == n) return false; }
    }
    return parts == 4;
}
static bool fmt_uuid(const char *s, size_t n) {
    if (n != 36) return false;
    for (size_t i = 0; i < n; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}
static bool fmt_email(const char *s, size_t n) {
    const char *at = memchr(s, '@', n);
    if (!at || at == s || memchr(at + 1, '@', n - (size_t)(at - s) - 1)) return false;
    const char *dom = at + 1; size_t dl = n - (size_t)(dom - s);
    if (dl < 3 || !memchr(dom, '.', dl) || dom[0] == '.' || dom[dl - 1] == '.') return false;
    for (size_t i = 0; i < n; i++) if (isspace((unsigned char)s[i])) return false;
    return true;
}
static bool fmt_uri(const char *s, size_t n) {
    size_t i = 0;
    if (n == 0 || !isalpha((unsigned char)s[0])) return false;
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')) i++;
    if (i + 1 > n || s[i] != ':' || i + 1 == n) return false;
    for (size_t k = 0; k < n; k++) if (isspace((unsigned char)s[k])) return false;
    return true;
}

static KcValue *resolve_ref(KcValue root, const char *ref) {
    if (ref[0] != '#') return NULL;
    if (ref[1] == 0) { static KcValue tmp; tmp = root; return &tmp; }
    if (ref[1] != '/') return NULL;
    PPath p; char err[100];
    if (!jpath_parse(ref + 1, strlen(ref + 1), &p, err, sizeof err)) return NULL;
    return jpath_resolve(root, &p);
}

static void validate_into(KcInterp *ip, KcPos pos, KcValue d, KcValue schema, KcValue root, const char *path, KcValue errors, int depth);

static bool passes(KcInterp *ip, KcPos pos, KcValue d, KcValue schema, KcValue root, const char *path, int depth) {
    KcValue tmp = kc_array_new();
    validate_into(ip, pos, d, schema, root, path, tmp, depth + 1);
    return tmp.as.arr->count == 0;
}

static char *child_path(const char *path, const char *key, size_t kl) {
    JBuf b; jb_init(&b);
    jb_puts(&b, path);
    jb_putc(&b, '/');
    for (size_t i = 0; i < kl; i++) { if (key[i] == '~') jb_puts(&b, "~0"); else if (key[i] == '/') jb_puts(&b, "~1"); else jb_putc(&b, key[i]); }
    size_t l; return jb_take(&b, &l);
}
static char *index_path(const char *path, int i) {
    JBuf b; jb_init(&b);
    jb_puts(&b, path); jb_printf(&b, "/%d", i);
    size_t l; return jb_take(&b, &l);
}

static void validate_into(KcInterp *ip, KcPos pos, KcValue d, KcValue schema, KcValue root, const char *path, KcValue errors, int depth) {
    if (depth > 64) kc_error_at(pos, "json.validate: schema nesting/$ref depth exceeded (recursive schema?)");
    if (schema.type == VAL_BOOL) {
        if (!schema.as.b) add_err(errors, path, "no value is allowed here (schema is false)");
        return;
    }
    if (schema.type != VAL_STRUCT) kc_error_at(pos, "json.validate: a schema must be an object or boolean, got %s", kc_type_name(schema));
    KcValue *k;

    if ((k = sk(schema, "$ref")) && k->type == VAL_STRING) {
        KcValue *target = resolve_ref(root, k->as.s->data);
        if (!target) kc_error_at(pos, "json.validate: cannot resolve $ref \"%s\" (only local refs like \"#/definitions/name\" are supported)", k->as.s->data);
        validate_into(ip, pos, d, *target, root, path, errors, depth + 1);
    }

    if ((k = sk(schema, "type"))) {
        bool ok = false;
        char want[120] = "";
        if (k->type == VAL_STRING) { ok = type_matches(d, k->as.s->data); snprintf(want, sizeof want, "%s", k->as.s->data); }
        else if (k->type == VAL_ARRAY) {
            for (int i = 0; i < k->as.arr->count; i++) {
                KcValue t = k->as.arr->items[i];
                if (t.type != VAL_STRING) continue;
                if (type_matches(d, t.as.s->data)) ok = true;
                if (strlen(want) + t.as.s->len + 4 < sizeof want) { if (want[0]) strcat(want, " or "); strcat(want, t.as.s->data); }
            }
        }
        if (!ok) add_err(errors, path, "expected %s, got %s", want[0] ? want : "a valid type", schema_type_name(d));
    }
    if ((k = sk(schema, "enum")) && k->type == VAL_ARRAY) {
        bool found = false;
        for (int i = 0; i < k->as.arr->count && !found; i++) found = jv_equal(d, k->as.arr->items[i]);
        if (!found) add_err(errors, path, "value is not one of the allowed enum values");
    }
    if ((k = sk(schema, "const")) && !jv_equal(d, *k)) add_err(errors, path, "value does not equal the required constant");

    if (is_num(d)) {
        double x = num_of(d);
        if ((k = sk(schema, "minimum")) && is_num(*k) && x < num_of(*k)) add_err(errors, path, "%g is less than the minimum %g", x, num_of(*k));
        if ((k = sk(schema, "maximum")) && is_num(*k) && x > num_of(*k)) add_err(errors, path, "%g is greater than the maximum %g", x, num_of(*k));
        if ((k = sk(schema, "exclusiveMinimum")) && is_num(*k) && x <= num_of(*k)) add_err(errors, path, "%g must be greater than %g", x, num_of(*k));
        if ((k = sk(schema, "exclusiveMaximum")) && is_num(*k) && x >= num_of(*k)) add_err(errors, path, "%g must be less than %g", x, num_of(*k));
        if ((k = sk(schema, "multipleOf")) && is_num(*k) && num_of(*k) > 0) {
            double m = num_of(*k), q = x / m;
            bool ok = (d.type == VAL_INT && k->type == VAL_INT) ? (d.as.i % k->as.i == 0) : fabs(q - round(q)) < 1e-9;
            if (!ok) add_err(errors, path, "%g is not a multiple of %g", x, m);
        }
    }

    if (d.type == VAL_STRING) {
        int cp = utf8_count(d.as.s->data, d.as.s->len);
        if ((k = sk(schema, "minLength")) && k->type == VAL_INT && cp < k->as.i) add_err(errors, path, "string is shorter than %lld characters", (long long)k->as.i);
        if ((k = sk(schema, "maxLength")) && k->type == VAL_INT && cp > k->as.i) add_err(errors, path, "string is longer than %lld characters", (long long)k->as.i);
        if ((k = sk(schema, "pattern")) && k->type == VAL_STRING && !regx_test(ip, pos, k->as.s->data, d))
            add_err(errors, path, "string does not match the pattern %s", k->as.s->data);
        if ((k = sk(schema, "format")) && k->type == VAL_STRING) {
            const char *f = k->as.s->data; const char *s = d.as.s->data; size_t n = (size_t)d.as.s->len;
            bool ok = true;
            if (!strcmp(f, "email")) ok = fmt_email(s, n);
            else if (!strcmp(f, "ipv4")) ok = fmt_ipv4(s, n);
            else if (!strcmp(f, "uuid")) ok = fmt_uuid(s, n);
            else if (!strcmp(f, "date")) ok = fmt_date(s, n);
            else if (!strcmp(f, "date-time")) ok = fmt_datetime(s, n);
            else if (!strcmp(f, "uri")) ok = fmt_uri(s, n);
            if (!ok) add_err(errors, path, "string is not a valid %s", f);
        }
    }

    if (d.type == VAL_ARRAY) {
        KcArray *arr = d.as.arr;
        if ((k = sk(schema, "minItems")) && k->type == VAL_INT && arr->count < k->as.i) add_err(errors, path, "array has fewer than %lld items", (long long)k->as.i);
        if ((k = sk(schema, "maxItems")) && k->type == VAL_INT && arr->count > k->as.i) add_err(errors, path, "array has more than %lld items", (long long)k->as.i);
        if ((k = sk(schema, "uniqueItems")) && k->type == VAL_BOOL && k->as.b) {
            bool dup = false;
            for (int i = 0; i < arr->count && !dup; i++)
                for (int j = i + 1; j < arr->count && !dup; j++) dup = jv_equal(arr->items[i], arr->items[j]);
            if (dup) add_err(errors, path, "array items are not unique");
        }
        KcValue *pre = sk(schema, "prefixItems"), *items = sk(schema, "items");
        int start = 0;
        if (pre && pre->type == VAL_ARRAY) {
            int m = pre->as.arr->count < arr->count ? pre->as.arr->count : arr->count;
            for (int i = 0; i < m; i++) { char *cp = index_path(path, i); validate_into(ip, pos, arr->items[i], pre->as.arr->items[i], root, cp, errors, depth + 1); free(cp); }
            start = m;
            if (items && items->type != VAL_ARRAY)
                for (int i = start; i < arr->count; i++) { char *cp = index_path(path, i); validate_into(ip, pos, arr->items[i], *items, root, cp, errors, depth + 1); free(cp); }
        } else if (items && items->type == VAL_ARRAY) {                      /* tuple form (draft 7) */
            int m = items->as.arr->count < arr->count ? items->as.arr->count : arr->count;
            for (int i = 0; i < m; i++) { char *cp = index_path(path, i); validate_into(ip, pos, arr->items[i], items->as.arr->items[i], root, cp, errors, depth + 1); free(cp); }
            KcValue *extra = sk(schema, "additionalItems");
            if (extra) for (int i = m; i < arr->count; i++) { char *cp = index_path(path, i); validate_into(ip, pos, arr->items[i], *extra, root, cp, errors, depth + 1); free(cp); }
        } else if (items) {
            for (int i = 0; i < arr->count; i++) { char *cp = index_path(path, i); validate_into(ip, pos, arr->items[i], *items, root, cp, errors, depth + 1); free(cp); }
        }
        if ((k = sk(schema, "contains"))) {
            bool any = false;
            for (int i = 0; i < arr->count && !any; i++) any = passes(ip, pos, arr->items[i], *k, root, path, depth);
            if (!any) add_err(errors, path, "no array item matches the \"contains\" schema");
        }
    }

    if (d.type == VAL_STRUCT) {
        KcStructInst *o = d.as.strct;
        if ((k = sk(schema, "minProperties")) && k->type == VAL_INT && o->count < k->as.i) add_err(errors, path, "object has fewer than %lld properties", (long long)k->as.i);
        if ((k = sk(schema, "maxProperties")) && k->type == VAL_INT && o->count > k->as.i) add_err(errors, path, "object has more than %lld properties", (long long)k->as.i);
        if ((k = sk(schema, "required")) && k->type == VAL_ARRAY) {
            for (int i = 0; i < k->as.arr->count; i++) {
                KcValue r = k->as.arr->items[i];
                if (r.type == VAL_STRING && !kc_object_get(d, r.as.s->data, (size_t)r.as.s->len)) add_err(errors, path, "missing required property \"%s\"", r.as.s->data);
            }
        }
        KcValue *props = sk(schema, "properties"), *pprops = sk(schema, "patternProperties"), *addl = sk(schema, "additionalProperties");
        for (int i = 0; i < o->count; i++) {
            const char *name = o->names[i];
            size_t nl = strlen(name);
            bool matched = false;
            char *cp = child_path(path, name, nl);
            if (props && props->type == VAL_STRUCT) {
                KcValue *ps = kc_object_get(*props, name, nl);
                if (ps) { matched = true; validate_into(ip, pos, o->values[i], *ps, root, cp, errors, depth + 1); }
            }
            if (pprops && pprops->type == VAL_STRUCT) {
                for (int j = 0; j < pprops->as.strct->count; j++) {
                    if (regx_test(ip, pos, pprops->as.strct->names[j], kc_string(name))) {
                        matched = true;
                        validate_into(ip, pos, o->values[i], pprops->as.strct->values[j], root, cp, errors, depth + 1);
                    }
                }
            }
            if (!matched && addl) {
                if (addl->type == VAL_BOOL) { if (!addl->as.b) add_err(errors, cp, "additional property \"%s\" is not allowed", name); }
                else validate_into(ip, pos, o->values[i], *addl, root, cp, errors, depth + 1);
            }
            free(cp);
        }
    }

    if ((k = sk(schema, "allOf")) && k->type == VAL_ARRAY)
        for (int i = 0; i < k->as.arr->count; i++) validate_into(ip, pos, d, k->as.arr->items[i], root, path, errors, depth + 1);
    if ((k = sk(schema, "anyOf")) && k->type == VAL_ARRAY) {
        bool any = false;
        for (int i = 0; i < k->as.arr->count && !any; i++) any = passes(ip, pos, d, k->as.arr->items[i], root, path, depth);
        if (!any) add_err(errors, path, "value does not match any of the %d \"anyOf\" alternatives", k->as.arr->count);
    }
    if ((k = sk(schema, "oneOf")) && k->type == VAL_ARRAY) {
        int hits = 0;
        for (int i = 0; i < k->as.arr->count; i++) if (passes(ip, pos, d, k->as.arr->items[i], root, path, depth)) hits++;
        if (hits == 0) add_err(errors, path, "value does not match any of the %d \"oneOf\" alternatives", k->as.arr->count);
        else if (hits > 1) add_err(errors, path, "value matches %d \"oneOf\" alternatives, but exactly one is required", hits);
    }
    if ((k = sk(schema, "not")) && passes(ip, pos, d, *k, root, path, depth)) add_err(errors, path, "value must not match the \"not\" schema");
    if ((k = sk(schema, "if"))) {
        if (passes(ip, pos, d, *k, root, path, depth)) { KcValue *t = sk(schema, "then"); if (t) validate_into(ip, pos, d, *t, root, path, errors, depth + 1); }
        else { KcValue *e = sk(schema, "else"); if (e) validate_into(ip, pos, d, *e, root, path, errors, depth + 1); }
    }
}

void js_validate(KcInterp *ip, KcPos pos, KcValue doc, KcValue schema, KcValue root_schema, const char *path, KcValue errors, int depth) {
    validate_into(ip, pos, doc, schema, root_schema, path, errors, depth);
}

/* ============================================================ module functions */

static void need(int n, int lo, int hi, KcPos pos, const char *sig) {
    if (n < lo || n > hi) kc_error_at(pos, "usage: json.%s", sig);
}

static KcValue schema_arg(KcValue s, KcPos pos, const char *who) {
    if (s.type != VAL_STRING) return s;                       /* schemas are never strings: treat as JSON text */
    JParseOpts o; json_default_parse_opts(&o);
    KcValue out; JError e;
    if (!json_parse_text(s.as.s->data, (size_t)s.as.s->len, &o, &out, &e))
        kc_error_at(pos, "%s: schema text is not valid JSON (line %d, column %d: %s)", who, e.line, e.col, e.msg);
    return out;
}

static KcValue s_validate(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "validate(doc, schema) -> string[]   (empty array = valid; schema may be an object or JSON text)");
    KcValue schema = schema_arg(a[1], pos, "json.validate");
    KcValue errors = kc_array_new();
    validate_into(ip, pos, a[0], schema, schema, "", errors, 0);
    return errors;
}
static KcValue s_matches(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "matches(doc, schema) -> bool");
    KcValue schema = schema_arg(a[1], pos, "json.matches");
    KcValue errors = kc_array_new();
    validate_into(ip, pos, a[0], schema, schema, "", errors, 0);
    return kc_bool(errors.as.arr->count == 0);
}

KcModuleEntry json_schema_entries[] = {
    {"validate", s_validate}, {"matches", s_matches},
};
const int json_schema_count = (int)(sizeof json_schema_entries / sizeof json_schema_entries[0]);
