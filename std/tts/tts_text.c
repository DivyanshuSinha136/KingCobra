/* KingCobra std.tts — text front end: Unicode clean-up, text normalisation (numbers, years, dates, times, currency,
 * units, abbreviations, acronyms, URLs/e-mail, camelCase), an SSML subset, inline {ARPAbet}/IPA phonemes and the
 * tokeniser that turns text into words + punctuation (with phonemes from tts_g2p.c).
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "tts_internal.h"
#include <ctype.h>
#include <stdarg.h>
#include <math.h>

/* ================================================================== buffer */
void tb_init(TBuf *b) { b->d = NULL; b->n = b->cap = 0; }
void tb_free(TBuf *b) { free(b->d); b->d = NULL; b->n = b->cap = 0; }
static void tb_grow(TBuf *b, size_t need) { if (b->n + need + 1 > b->cap) { b->cap = (b->cap + need + 1) * 2; b->d = realloc(b->d, b->cap); } }
void tb_putc(TBuf *b, char c) { tb_grow(b, 1); b->d[b->n++] = c; b->d[b->n] = 0; }
void tb_puts(TBuf *b, const char *s) { size_t l = strlen(s); tb_grow(b, l); memcpy(b->d + b->n, s, l + 1); b->n += l; }
void tb_printf(TBuf *b, const char *fmt, ...) { char t[512]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap); tb_puts(b, t); }
char *tb_take(TBuf *b) { if (!b->d) return strdup(""); char *r = b->d; b->d = NULL; b->n = b->cap = 0; return r; }
static void tb_word(TBuf *b, const char *w) { if (b->n && b->d[b->n - 1] != ' ') tb_putc(b, ' '); tb_puts(b, w); }

/* ================================================================== numbers */
static const char *const ONES[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen" };
static const char *const TENS[] = { "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety" };
static const struct { long long v; const char *n; } SCALES[] = {
    { 1000000000000000000LL, "quintillion" }, { 1000000000000000LL, "quadrillion" }, { 1000000000000LL, "trillion" }, { 1000000000LL, "billion" }, { 1000000LL, "million" }, { 1000LL, "thousand" } };

static void lt1000(TBuf *b, int v, bool andword, bool lead_and) {
    if (v >= 100) { tb_word(b, ONES[v / 100]); tb_word(b, "hundred"); v %= 100; if (v && andword) tb_word(b, "and"); }
    else if (lead_and && v && andword) tb_word(b, "and");
    if (v >= 20) { tb_word(b, TENS[v / 10]); if (v % 10) tb_word(b, ONES[v % 10]); }
    else if (v > 0) tb_word(b, ONES[v]);
}
void tts_cardinal_ll(TBuf *b, long long v, bool andword) {
    if (v < 0) { tb_word(b, "minus"); v = -v; }
    if (v == 0) { tb_word(b, "zero"); return; }
    bool any = false;
    for (size_t i = 0; i < sizeof SCALES / sizeof SCALES[0]; i++) {
        if (v >= SCALES[i].v) { long long q = v / SCALES[i].v; v %= SCALES[i].v; lt1000(b, (int)q, andword, false); tb_word(b, SCALES[i].n); any = true; }
    }
    if (v) lt1000(b, (int)v, andword, any);
}
static void make_ordinal_word(char *w) {
    size_t L = strlen(w);
    static const struct { const char *a, *b; } IRR[] = { { "one", "first" }, { "two", "second" }, { "three", "third" }, { "five", "fifth" }, { "eight", "eighth" }, { "nine", "ninth" }, { "twelve", "twelfth" } };
    for (size_t i = 0; i < sizeof IRR / sizeof IRR[0]; i++) if (!strcmp(w, IRR[i].a)) { strcpy(w, IRR[i].b); return; }
    if (L && w[L - 1] == 'y') { w[L - 1] = 0; strcat(w, "ieth"); return; }
    strcat(w, "th");
}
void tts_ordinal_ll(TBuf *b, long long v, bool andword) {
    TBuf t; tb_init(&t); tts_cardinal_ll(&t, v, andword);
    char *s = t.d; char *sp = strrchr(s, ' '); char last[40]; snprintf(last, sizeof last, "%s", sp ? sp + 1 : s);
    make_ordinal_word(last);
    if (sp) { *sp = 0; tb_word(b, s); }
    tb_word(b, last); tb_free(&t);
}
void tts_year_words(TBuf *b, int y) {
    if (y >= 2000 && y <= 2009) { tts_cardinal_ll(b, y, false); return; }
    int hi = y / 100, lo = y % 100;
    tts_cardinal_ll(b, hi, false);
    if (lo == 0) tb_word(b, "hundred");
    else if (lo < 10) { tb_word(b, "oh"); tb_word(b, ONES[lo]); }
    else tts_cardinal_ll(b, lo, false);
}
static void digits_words(TBuf *b, const char *d, int n, const char *zero) {
    for (int i = 0; i < n; i++) if (isdigit((unsigned char)d[i])) tb_word(b, d[i] == '0' ? zero : ONES[d[i] - '0']);
}
typedef struct { bool neg; char ip[48]; int ipn; char fp[48]; int fpn; bool has_dec, has_comma; const char *end; } Num;
static bool parse_num(const char *s, Num *n) {
    memset(n, 0, sizeof *n); const char *p = s;
    if ((*p == '-' || *p == '+') && isdigit((unsigned char)p[1])) { n->neg = *p == '-'; p++; }
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p) || (*p == ',' && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) && !isdigit((unsigned char)p[4]))) {
        if (*p == ',') { n->has_comma = true; p++; continue; }
        if (n->ipn < 46) n->ip[n->ipn++] = *p; p++;
    }
    if (*p == '.' && isdigit((unsigned char)p[1])) { n->has_dec = true; p++; while (isdigit((unsigned char)*p)) { if (n->fpn < 46) n->fp[n->fpn++] = *p; p++; } }
    n->end = p; return true;
}
static void num_cardinal(TBuf *b, const Num *n, const TtsOpts *o, bool year_ok) {
    if (n->neg) tb_word(b, "minus");
    if (n->ipn > 18 || (n->ipn > 1 && n->ip[0] == '0' && !n->has_dec)) { digits_words(b, n->ip, n->ipn, "zero"); }
    else {
        long long v = strtoll(n->ip, NULL, 10);
        if (year_ok && !n->has_dec && !n->has_comma && n->ipn == 4 && v >= 1100 && v <= 2099 && v % 1000 != 0 && !(o && o->year_cardinal)) tts_year_words(b, (int)v);
        else tts_cardinal_ll(b, v, o && o->and_word);
    }
    if (n->has_dec) { tb_word(b, "point"); digits_words(b, n->fp, n->fpn, "zero"); }
}

/* ================================================================== tables */
static const char *const MONTHS[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
typedef struct { const char *ab, *full; bool drop_dot; bool strict; } Abbr;
static const Abbr ABBR[] = {
    { "mr", "mister", true, false }, { "mrs", "missus", true, false }, { "ms", "miz", true, false }, { "dr", "doctor", true, false }, { "prof", "professor", true, false }, { "sr", "senior", true, false }, { "jr", "junior", true, false }, { "st", "saint", true, true },
    { "vs", "versus", true, false }, { "etc", "et cetera", false, false }, { "approx", "approximately", false, false }, { "inc", "incorporated", false, true }, { "ltd", "limited", false, true }, { "co", "company", false, true }, { "corp", "corporation", false, true },
    { "fig", "figure", true, true }, { "mt", "mount", true, true }, { "ave", "avenue", true, true }, { "blvd", "boulevard", true, false }, { "rd", "road", true, true }, { "dept", "department", true, true }, { "univ", "university", true, true },
    { "gen", "general", true, true }, { "gov", "governor", true, true }, { "sen", "senator", true, true }, { "rep", "representative", true, true }, { "capt", "captain", true, true }, { "col", "colonel", true, true }, { "lt", "lieutenant", true, true },
    { "sgt", "sergeant", true, true }, { "cmdr", "commander", true, true }, { "rev", "reverend", true, true }, { "hon", "honorable", true, true }, { "esq", "esquire", true, false }, { "min", "minutes", false, true }, { "est", "established", false, true },
    { "jan", "January", true, true }, { "feb", "February", true, true }, { "mar", "March", true, true }, { "apr", "April", true, true }, { "jun", "June", true, true }, { "jul", "July", true, true }, { "aug", "August", true, true }, { "sep", "September", true, true }, { "sept", "September", true, true },
    { "oct", "October", true, true }, { "nov", "November", true, true }, { "dec", "December", true, true }, { "mon", "Monday", true, true }, { "tue", "Tuesday", true, true }, { "tues", "Tuesday", true, true }, { "wed", "Wednesday", true, true }, { "thu", "Thursday", true, true },
    { "thur", "Thursday", true, true }, { "thurs", "Thursday", true, true }, { "fri", "Friday", true, true }, { "sat", "Saturday", true, true }, { "sun", "Sunday", true, true }, { "e.g", "for example", true, false }, { "i.e", "that is", true, false }, { "cf", "compare", true, true },
    { "ca", "circa", true, true }, { "al", "al", false, true }, { "tbd", "to be determined", false, false }, { "faq", "F A Q", false, false }, { "diy", "D I Y", false, false }, { "asap", "A S A P", false, false }, { "aka", "also known as", false, false }, { "p.s", "P S", true, false }, { "ph.d", "P H D", true, false },
    { NULL, NULL, false, false }
};
typedef struct { const char *ab, *one, *many; bool attach_only; } Unit;
static const Unit UNITS[] = {
    { "km", "kilometer", "kilometers", false }, { "cm", "centimeter", "centimeters", false }, { "mm", "millimeter", "millimeters", false }, { "m", "meter", "meters", true }, { "kg", "kilogram", "kilograms", false },
    { "mg", "milligram", "milligrams", false }, { "g", "gram", "grams", true }, { "lb", "pound", "pounds", false }, { "lbs", "pound", "pounds", false }, { "oz", "ounce", "ounces", false }, { "mph", "mile per hour", "miles per hour", false },
    { "kph", "kilometer per hour", "kilometers per hour", false }, { "kb", "kilobyte", "kilobytes", false }, { "mb", "megabyte", "megabytes", false }, { "gb", "gigabyte", "gigabytes", false }, { "tb", "terabyte", "terabytes", false },
    { "hz", "hertz", "hertz", false }, { "khz", "kilohertz", "kilohertz", false }, { "mhz", "megahertz", "megahertz", false }, { "ghz", "gigahertz", "gigahertz", false }, { "kw", "kilowatt", "kilowatts", false },
    { "ms", "millisecond", "milliseconds", false }, { "ns", "nanosecond", "nanoseconds", false }, { "ft", "foot", "feet", false }, { "mi", "mile", "miles", true }, { "ml", "milliliter", "milliliters", false },
    { "hr", "hour", "hours", false }, { "hrs", "hour", "hours", false }, { "sec", "second", "seconds", false }, { "kbps", "kilobit per second", "kilobits per second", false }, { "mbps", "megabit per second", "megabits per second", false },
    { "gbps", "gigabit per second", "gigabits per second", false }, { "px", "pixel", "pixels", false }, { "fps", "frame per second", "frames per second", false }, { "bpm", "beat per minute", "beats per minute", false }, { "db", "decibel", "decibels", false }, { "km/h", "kilometer per hour", "kilometers per hour", false }, { "m/s", "meter per second", "meters per second", false }, { "kmh", "kilometer per hour", "kilometers per hour", false },
    { NULL, NULL, NULL, false }
};
static const Unit *find_unit(const char *s, size_t len, bool attached) {
    for (const Unit *u = UNITS; u->ab; u++) if (strlen(u->ab) == len && !strncasecmp(u->ab, s, len) && (attached || !u->attach_only)) return u;
    return NULL;
}
static const char *const CUR_ONE[] = { "dollar", "pound", "euro", "yen", "rupee" };
static const char *const CUR_MANY[] = { "dollars", "pounds", "euros", "yen", "rupees" };
static const char *const CUR_SUB1[] = { "cent", "penny", "cent", "", "paisa" };
static const char *const CUR_SUBN[] = { "cents", "pence", "cents", "", "paise" };
static int cur_index(char c) { switch (c) { case '$': return 0; case '\x11': return 1; case '\x12': return 2; case '\x13': return 3; case '\x14': return 4; } return -1; }
static bool is_scale_word(const char *w, long long *mult) {
    static const struct { const char *w; long long m; } T[] = { { "thousand", 1000 }, { "million", 1000000 }, { "billion", 1000000000LL }, { "trillion", 1000000000000LL }, { "k", 1000 }, { "m", 1000000 }, { "b", 1000000000LL }, { "bn", 1000000000LL }, { "t", 1000000000000LL }, { "lakh", 100000 }, { "crore", 10000000 } };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcasecmp(w, T[i].w)) { *mult = T[i].m; return true; }
    return false;
}
static const char *scale_name(long long m) {
    switch (m) { case 1000: return "thousand"; case 1000000: return "million"; case 1000000000LL: return "billion"; case 1000000000000LL: return "trillion"; case 100000: return "lakh"; case 10000000: return "crore"; }
    return "";
}
static int month_of(const char *w) { for (int i = 0; i < 12; i++) if (!strcasecmp(w, MONTHS[i])) return i + 1; return 0; }

/* ================================================================== unicode clean-up */
static const char LAT1_LOWER[] = "aaaaaaaceeeeiiiidnooooo/ouuuuyts";   /* U+00E0..U+00FF */
static char *clean_text(const char *in) {
    TBuf b; tb_init(&b);
    for (const unsigned char *p = (const unsigned char *)in; *p; ) {
        unsigned c = *p;
        if (c < 0x80) {
            if (c == '\r') { p++; continue; }
            if (c == '\n' || c == '\t' || (c >= 32 && c != 127)) tb_putc(&b, (char)c);
            p++; continue;
        }
        unsigned cp; int len;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; } else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; } else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; } else { p++; continue; }
        int k = 1; for (; k < len && (p[k] & 0xC0) == 0x80; k++) cp = (cp << 6) | (p[k] & 0x3F);
        p += k; if (k < len) continue;
        switch (cp) {
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0x00B4: case 0x0060: tb_putc(&b, '\''); break;
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: case 0x00AB: case 0x00BB: tb_putc(&b, '"'); break;
        case 0x2013: case 0x2014: case 0x2015: tb_puts(&b, " - "); break;
        case 0x2212: tb_putc(&b, '-'); break;
        case 0x2026: tb_puts(&b, "..."); break;
        case 0x00A0: case 0x2009: case 0x200A: case 0x202F: case 0x3000: tb_putc(&b, ' '); break;
        case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: break;
        case 0x00A3: tb_putc(&b, '\x11'); break; case 0x20AC: tb_putc(&b, '\x12'); break; case 0x00A5: tb_putc(&b, '\x13'); break; case 0x20B9: tb_putc(&b, '\x14'); break;
        case 0x00B0: tb_putc(&b, '\x15'); break; case 0x00D7: tb_putc(&b, '\x16'); break; case 0x00F7: tb_putc(&b, '\x17'); break;
        case 0x00BD: tb_puts(&b, " 1/2 "); break; case 0x00BC: tb_puts(&b, " 1/4 "); break; case 0x00BE: tb_puts(&b, " 3/4 "); break;
        case 0x00DF: tb_puts(&b, "ss"); break; case 0x00E6: tb_puts(&b, "ae"); break; case 0x00C6: tb_puts(&b, "AE"); break; case 0x0153: tb_puts(&b, "oe"); break; case 0x00F8: tb_putc(&b, 'o'); break; case 0x00D8: tb_putc(&b, 'O'); break;
        case 0x2022: case 0x00B7: tb_puts(&b, " , "); break;
        default:
            if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) { char l = LAT1_LOWER[cp - 0x00C0]; tb_putc(&b, (char)toupper((unsigned char)l)); }
            else if (cp >= 0x00E0 && cp <= 0x00FF && cp != 0x00F7) tb_putc(&b, LAT1_LOWER[cp - 0x00E0]);
            else if (cp >= 0x0100 && cp <= 0x017F) { static const char L2[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs"; size_t ix = cp - 0x100; tb_putc(&b, ix < sizeof L2 - 1 ? L2[ix] : ' '); }
            else tb_putc(&b, ' ');
        }
    }
    return tb_take(&b);
}

/* ================================================================== normalisation */
typedef struct { const TtsOpts *o; char prevw[64]; bool had_dot; } Ctx;
static bool is_alpha_s(const char *s) { if (!*s) return false; for (; *s; s++) if (!isalpha((unsigned char)*s)) return false; return true; }
static bool all_upper(const char *s) { bool any = false; for (; *s; s++) { if (islower((unsigned char)*s)) return false; if (isupper((unsigned char)*s)) any = true; } return any; }
static void emit_spelled(TBuf *b, const char *s, size_t n) { tb_word(b, "\x01"); for (size_t i = 0; i < n; i++) tb_putc(b, s[i]); tb_putc(b, '\x02'); }

static void say_time(TBuf *b, int h, int m, int sec, const char *ampm, bool has_ampm) {
    int hh = h;
    if (has_ampm || (h >= 1 && h <= 12)) { if (hh > 12) hh -= 12; if (hh == 0) hh = 12; }
    tts_cardinal_ll(b, hh, false);
    if (m == 0 && !has_ampm && h > 12) tb_word(b, "hundred hours");
    else if (m == 0 && !has_ampm) tb_word(b, "o'clock");
    else if (m > 0) { if (m < 10) tb_word(b, "oh"); tts_cardinal_ll(b, m, false); }
    if (sec > 0) { tb_word(b, "and"); tts_cardinal_ll(b, sec, false); tb_word(b, sec == 1 ? "second" : "seconds"); }
    if (has_ampm) { emit_spelled(b, ampm[0] == 'p' || ampm[0] == 'P' ? "pm" : "am", 2); }
}
static void say_date(TBuf *b, int mo, int day, int year, const TtsOpts *o) {
    if (mo >= 1 && mo <= 12) tb_word(b, MONTHS[mo - 1]);
    if (day > 0) tts_ordinal_ll(b, day, false);
    if (year > 0) { if (year < 100) { tb_word(b, year < 50 ? "twenty" : "nineteen"); if (year) tts_cardinal_ll(b, year, false); } else tts_year_words(b, year); }
    (void)o;
}
static void say_currency(TBuf *b, int ci, const Num *n, long long mult, const TtsOpts *o) {
    if (n->neg) tb_word(b, "minus");
    long long whole = strtoll(n->ip, NULL, 10);
    if (mult > 1) {
        if (n->has_dec) { Num t = *n; t.neg = false; num_cardinal(b, &t, o, false); } else tts_cardinal_ll(b, whole, false);
        tb_word(b, scale_name(mult)); tb_word(b, CUR_MANY[ci]); return;
    }
    int cents = 0; if (n->has_dec) { char t[3] = "00"; for (int i = 0; i < 2 && i < n->fpn; i++) t[i] = n->fp[i]; cents = atoi(t); }
    if (whole > 0 || cents == 0) { tts_cardinal_ll(b, whole, o && o->and_word); tb_word(b, whole == 1 ? CUR_ONE[ci] : CUR_MANY[ci]); }
    if (cents > 0 && CUR_SUB1[ci][0]) { if (whole > 0 || 0) tb_word(b, "and"); tts_cardinal_ll(b, cents, false); tb_word(b, cents == 1 ? CUR_SUB1[ci] : CUR_SUBN[ci]); }
}

/* core token (no leading/trailing punctuation). Returns number of following tokens consumed (0/1). */
static int norm_core(Ctx *c, const char *core, const char *nxt, TBuf *out) {
    size_t L = strlen(core); if (!L) return 0;
    const TtsOpts *o = c->o;
    /* paragraph / markers pass through */
    if (core[0] == '\x03') { tb_word(out, "\x03"); return 0; }

    /* ---- URLs and e-mail ---- */
    if (strstr(core, "://") || !strncasecmp(core, "www.", 4) || (strchr(core, '@') && strchr(strchr(core, '@'), '.') && L > 5)) {
        const char *p = core; const char *s3 = strstr(core, "://"); if (s3) p = s3 + 3;
        if (!strncasecmp(p, "www.", 4)) { emit_spelled(out, "www", 3); tb_word(out, "dot"); p += 4; }
        char seg[128]; int k = 0;
        for (;; p++) {
            char ch = *p;
            if (ch && (isalnum((unsigned char)ch) || ch == '\'')) { if (k < 126) seg[k++] = ch; continue; }
            if (k) { seg[k] = 0; Ctx c2 = *c; norm_core(&c2, seg, NULL, out); k = 0; }
            if (!ch) break;
            switch (ch) { case '.': tb_word(out, "dot"); break; case '/': tb_word(out, "slash"); break; case '@': tb_word(out, "at"); break; case '-': tb_word(out, "dash"); break;
                          case '_': tb_word(out, "underscore"); break; case ':': break; case '?': tb_word(out, "question mark"); break; case '=': tb_word(out, "equals"); break; case '&': tb_word(out, "and"); break; case '#': tb_word(out, "hash"); break; default: break; }
        }
        return 0;
    }
    /* ---- currency (prefix symbol) ---- */
    int ci = cur_index(core[0]);
    if (ci >= 0 && L > 1) {
        Num n; if (parse_num(core + 1, &n)) {
            long long mult = 1; const char *e = n.end; int cons = 0;
            if (*e) { if (is_scale_word(e, &mult) && strcasecmp(e, "t") != 0) {} else if (*e) mult = 1, e = n.end; }
            if (mult == 1 && !*n.end && nxt && is_scale_word(nxt, &mult) && strlen(nxt) > 1) cons = 1; else if (mult == 1 && !*n.end) mult = 1;
            if (!*n.end || mult > 1) { say_currency(out, ci, &n, mult, o); return cons; }
        }
    }
    if ((!strcasecmp(core, "rs") || !strcasecmp(core, "rs.")) && nxt && isdigit((unsigned char)nxt[0])) { Num n; if (parse_num(nxt, &n) && !*n.end) { say_currency(out, 4, &n, 1, o); return 1; } }
    /* ---- abbreviations (before numbers so "No. 5" works) ---- */
    {
        char low[64]; size_t k = 0; for (; core[k] && k < 63; k++) low[k] = (char)tolower((unsigned char)core[k]); low[k] = 0;
        if (!strcmp(low, "no") && nxt && isdigit((unsigned char)nxt[0]) && core[0] == 'N') { tb_word(out, "number"); return 0; }
        bool cap = isupper((unsigned char)core[0]), next_num = nxt && isdigit((unsigned char)nxt[0]);
        for (const Abbr *a = ABBR; a->ab; a++) {
            if (strcmp(low, a->ab)) continue;
            bool ok;
            if (a->strict) ok = c->had_dot || (cap && next_num);          /* also ordinary words: only with a period, or "Oct 28" */
            else ok = cap || c->had_dot || strchr(a->ab, '.') || !strcmp(a->ab, "etc") || !strcmp(a->ab, "vs") || !strcmp(a->ab, "approx");
            if (!ok) break;
            if (!strcmp(a->ab, "st") && (!nxt || islower((unsigned char)nxt[0]))) { tb_word(out, "street"); return 0; }
            if (a->full[1] == ' ' && isupper((unsigned char)a->full[0]) && strlen(a->full) <= 6) { for (const char *q = a->full; *q; q++) if (isalpha((unsigned char)*q)) emit_spelled(out, q, 1); return 0; }
            tb_word(out, a->full); return 0;
        }
    }
    /* ---- dotted abbreviations / initialisms: U.S.A  a.m  p.m  ph.d ---- */
    if (strchr(core, '.') && L >= 3) {
        bool alt = true; for (size_t i = 0; i < L; i++) { if (i % 2 == 0 ? !isalpha((unsigned char)core[i]) : core[i] != '.') { if (!(i == L - 1 && isalpha((unsigned char)core[i]))) alt = false; } }
        if (alt && L <= 9) { char lt[12]; int k = 0; for (size_t i = 0; i < L; i++) if (isalpha((unsigned char)core[i])) lt[k++] = core[i]; lt[k] = 0; emit_spelled(out, lt, (size_t)k); return 0; }
    }
    /* ---- numeric forms ---- */
    if (isdigit((unsigned char)core[0]) || ((core[0] == '-' || core[0] == '+' || core[0] == '~') && isdigit((unsigned char)core[1]))) {
        const char *s = core;
        if (*s == '~') { tb_word(out, "about"); s++; }
        /* time h:mm[:ss][am|pm] */
        { int h, m, sec = 0, nch = 0; if (sscanf(s, "%d:%d%n", &h, &m, &nch) == 2 && h >= 0 && h < 25 && m >= 0 && m < 60 && isdigit((unsigned char)s[nch - 1])) {
              const char *e = s + nch; if (*e == ':' && isdigit((unsigned char)e[1])) { sec = atoi(e + 1); e += 3; }
              bool ap = false; const char *apm = ""; int cons = 0;
              if (!strncasecmp(e, "am", 2) || !strncasecmp(e, "pm", 2)) { ap = true; apm = e; e += 2; }
              else if (!strncasecmp(e, "a.m", 3) || !strncasecmp(e, "p.m", 3)) { ap = true; apm = e; e += 3; if (*e == '.') e++; }
              else if (!*e && nxt && (!strcasecmp(nxt, "am") || !strcasecmp(nxt, "pm") || !strcasecmp(nxt, "a.m.") || !strcasecmp(nxt, "p.m.") || !strcasecmp(nxt, "a.m") || !strcasecmp(nxt, "p.m"))) { ap = true; apm = nxt; cons = 1; }
              if (!*e) { say_time(out, h, m, sec, apm, ap); return cons; } } }
        /* am/pm after plain hour: "5pm" */
        { Num n; if (parse_num(s, &n) && !n.has_dec && n.ipn <= 2 && (!strcasecmp(n.end, "am") || !strcasecmp(n.end, "pm"))) { int h = atoi(n.ip); if (h >= 1 && h <= 12) { tts_cardinal_ll(out, h, false); emit_spelled(out, n.end, 2); return 0; } } }
        /* ISO date */
        { int y, mo, d, nch = 0; if (sscanf(s, "%d-%d-%d%n", &y, &mo, &d, &nch) == 3 && y >= 1000 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && !s[nch]) { say_date(out, mo, d, y, o); return 0; } }
        /* slash date or fraction */
        { int a, b2, y = 0, nch = 0; int k = sscanf(s, "%d/%d/%d%n", &a, &b2, &y, &nch);
          if (k == 3 && !s[nch]) { int mo = o && o->dateorder_dmy ? b2 : a, d = o && o->dateorder_dmy ? a : b2; if (mo >= 1 && mo <= 12 && d >= 1 && d <= 31) { say_date(out, mo, d, y, o); return 0; } }
          nch = 0; k = sscanf(s, "%d/%d%n", &a, &b2, &nch);
          if (k == 2 && !s[nch] && b2 >= 2 && b2 <= 100 && a >= 0) {
              tts_cardinal_ll(out, a, false);
              if (b2 == 2) tb_word(out, a == 1 ? "half" : "halves"); else if (b2 == 4) tb_word(out, a == 1 ? "quarter" : "quarters");
              else { TBuf t; tb_init(&t); tts_ordinal_ll(&t, b2, false); tb_word(out, t.d); if (a != 1) tb_putc(out, 's'); tb_free(&t); }
              return 0; } }
        /* phone-like: digits with dashes/spaces */
        { int digs = 0, groups = 0, gl = 0; bool ok = true; for (const char *q = s; *q; q++) { if (isdigit((unsigned char)*q)) { digs++; gl++; } else if (*q == '-') { if (gl) groups++; gl = 0; } else if (*q != '+') { ok = false; break; } }
          if (gl) groups++;
          if (ok && groups >= 2 && digs >= 7 && strchr(s, '-')) { bool first = true; const char *q = s; if (*q == '+') { tb_word(out, "plus"); q++; }
              while (*q) { char g[16]; int k = 0; while (*q && *q != '-' && k < 15) g[k++] = *q++; g[k] = 0; if (*q == '-') q++; if (!first) tb_puts(out, ","); first = false; digits_words(out, g, k, "oh"); }
              return 0; } }
        /* range 5-10, 1990-1999 */
        { Num a; if (parse_num(s, &a) && *a.end == '-' && isdigit((unsigned char)a.end[1])) { Num b2; if (parse_num(a.end + 1, &b2) && !*b2.end) { num_cardinal(out, &a, o, a.ipn == 4); tb_word(out, "to"); num_cardinal(out, &b2, o, b2.ipn == 4); return 0; } } }
        Num n; if (!parse_num(s, &n)) { digits_words(out, s, (int)strlen(s), "zero"); return 0; }
        const char *e = n.end;
        /* long digit strings -> grouped digits */
        if (!n.has_comma && !n.has_dec && n.ipn >= 9 && !*e) { for (int i = 0; i < n.ipn; i += 3) { if (i) tb_puts(out, ","); digits_words(out, n.ip + i, n.ipn - i < 3 ? n.ipn - i : 3, "oh"); } return 0; }
        /* ordinals */
        if (!n.has_dec && (!strcasecmp(e, "st") || !strcasecmp(e, "nd") || !strcasecmp(e, "rd") || !strcasecmp(e, "th")) && n.ipn <= 18) { tts_ordinal_ll(out, strtoll(n.ip, NULL, 10), o && o->and_word); return 0; }
        /* decades: 1990s, 80s */
        if (!n.has_dec && !strcasecmp(e, "s") && n.ip[n.ipn - 1] == '0' && (n.ipn == 2 || n.ipn == 4)) {
            TBuf t; tb_init(&t); if (n.ipn == 4) tts_year_words(&t, atoi(n.ip)); else tts_cardinal_ll(&t, atoi(n.ip), false);
            char *sp = strrchr(t.d, ' '); char last[40]; snprintf(last, sizeof last, "%s", sp ? sp + 1 : t.d);
            size_t ll = strlen(last); if (ll && last[ll - 1] == 'y') { last[ll - 1] = 0; strcat(last, "ies"); } else strcat(last, "s");
            if (sp) { *sp = 0; tb_word(out, t.d); } tb_word(out, last); tb_free(&t); return 0; }
        if (n.neg) { Num t = n; t.neg = false; tb_word(out, "minus"); n = t; }
        else if (core[0] == '+' ) tb_word(out, "plus");
        /* suffixes */
        if (*e == '%' ) { num_cardinal(out, &n, o, false); tb_word(out, "percent"); return 0; }
        if (*e == '\x15') { num_cardinal(out, &n, o, false); tb_word(out, "degrees"); e++; if (*e == 'C' || *e == 'c') tb_word(out, "Celsius"); else if (*e == 'F' || *e == 'f') tb_word(out, "Fahrenheit"); return 0; }
        if ((*e == 'x' || *e == 'X') && !e[1]) { num_cardinal(out, &n, o, false); tb_word(out, "times"); return 0; }
        if (*e == 'k' || *e == 'K') { if (!e[1]) { num_cardinal(out, &n, o, false); tb_word(out, "thousand"); return 0; } }
        if (*e) { const Unit *u = find_unit(e, strlen(e), true);
            if (u) { bool one = !n.has_dec && n.ipn == 1 && n.ip[0] == '1'; num_cardinal(out, &n, o, false); tb_word(out, one ? u->one : u->many); return 0; }
            if (isalpha((unsigned char)*e)) {   /* alphanumeric mix: 3D, 5G, 4K, 2nd handled above */
                num_cardinal(out, &n, o, false);
                if (strlen(e) <= 3 && all_upper(e)) emit_spelled(out, e, strlen(e)); else { Ctx c2 = *c; norm_core(&c2, e, NULL, out); }
                return 0; } }
        /* number followed by a unit / percent word token */
        int cons = 0;
        if (!*e && nxt) { const Unit *u = find_unit(nxt, strlen(nxt), false);
            if (u && strlen(nxt) >= 2) { bool one = !n.has_dec && n.ipn == 1 && n.ip[0] == '1'; num_cardinal(out, &n, o, false); tb_word(out, one ? u->one : u->many); return 1; }
            if (!strcmp(nxt, "%")) { num_cardinal(out, &n, o, false); tb_word(out, "percent"); return 1; } }
        bool year_ok = !(c->prevw[0] && (!strcmp(c->prevw, "page") || !strcmp(c->prevw, "chapter") || !strcmp(c->prevw, "room") || !strcmp(c->prevw, "version") || !strcmp(c->prevw, "line") || !strcmp(c->prevw, "number")));
        if (!n.has_comma && !n.has_dec && n.ipn == 4 && nxt) { /* "1000 people" vs "in 1999": numbers followed by a plural-looking noun are quantities */
            size_t nl = strlen(nxt); if (nl > 3 && nxt[nl - 1] == 's' && nxt[nl - 2] != 's' && !month_of(nxt) && strcasecmp(nxt, "was") && strcasecmp(nxt, "is") && strcasecmp(nxt, "has") && strcasecmp(nxt, "as") && strcasecmp(nxt, "its")) year_ok = false; }
        num_cardinal(out, &n, o, year_ok);
        if (*e == '.' && !e[1]) {}
        return cons;
    }
    /* ---- symbols standing alone ---- */
    if (L == 1 || (L == 2 && core[0] == core[1])) {
        switch (core[0]) {
        case '&': tb_word(out, "and"); return 0; case '+': tb_word(out, "plus"); return 0; case '=': tb_word(out, "equals"); return 0; case '<': tb_word(out, "less than"); return 0; case '>': tb_word(out, "greater than"); return 0;
        case '@': tb_word(out, "at"); return 0; case '#': tb_word(out, "hash"); return 0; case '%': tb_word(out, "percent"); return 0; case '*': tb_word(out, "star"); return 0; case '/': tb_word(out, "slash"); return 0;
        case '\\': tb_word(out, "backslash"); return 0; case '^': tb_word(out, "caret"); return 0; case '~': tb_word(out, "tilde"); return 0; case '|': tb_word(out, "bar"); return 0; case '\x15': tb_word(out, "degrees"); return 0;
        case '\x16': tb_word(out, "times"); return 0; case '\x17': tb_word(out, "divided by"); return 0; case '$': tb_word(out, "dollar"); return 0; case '_': return 0;
        default: break; }
    }
    if (core[0] == '#' && L > 1) { Num n; if (parse_num(core + 1, &n) && !*n.end) { tb_word(out, "number"); num_cardinal(out, &n, o, false); return 0; } tb_word(out, "hash"); Ctx c2 = *c; norm_core(&c2, core + 1, nxt, out); return 0; }
    if (core[0] == '@' && L > 1 && isalpha((unsigned char)core[1])) { tb_word(out, "at"); Ctx c2 = *c; norm_core(&c2, core + 1, nxt, out); return 0; }
    /* ---- emphasis markers *word* ---- */
    if (core[0] == '*' && L > 2 && core[L - 1] == '*') { char t[128]; snprintf(t, sizeof t, "%.*s", (int)L - 2, core + 1); tb_word(out, "\x05"); Ctx c2 = *c; norm_core(&c2, t, NULL, out); tb_putc(out, '\x06'); return 0; }
    /* ---- compound with internal separators: a_b, a/b, a.b, a+b, a&b ---- */
    for (size_t i = 1; i + 1 < L; i++) {
        char ch = core[i]; if (!(ch == '_' || ch == '/' || ch == '&' || ch == '+' || ch == '=' || ch == '.' || ch == '\\' || ch == '|' || ch == '@' || ch == '*' || ch == '<' || ch == '>')) continue;
        if (ch == '.' && isdigit((unsigned char)core[i - 1]) && isdigit((unsigned char)core[i + 1])) continue;
        char l[128], r[128]; snprintf(l, sizeof l, "%.*s", (int)i, core); snprintf(r, sizeof r, "%s", core + i + 1);
        Ctx c2 = *c; norm_core(&c2, l, NULL, out);
        switch (ch) { case '.': tb_word(out, "dot"); break; case '/': tb_word(out, "slash"); break; case '&': tb_word(out, "and"); break; case '+': tb_word(out, "plus"); break; case '=': tb_word(out, "equals"); break;
                      case '@': tb_word(out, "at"); break; case '\\': tb_word(out, "backslash"); break; case '|': tb_word(out, "bar"); break; case '*': tb_word(out, "star"); break; case '<': tb_word(out, "less than"); break; case '>': tb_word(out, "greater than"); break; default: break; }
        norm_core(&c2, r, nxt, out); return 0;
    }
    /* ---- letters + digits ("mp3", "H2O", "Win11") ---- */
    bool has_d = false, has_a = false; for (size_t i = 0; i < L; i++) { if (isdigit((unsigned char)core[i])) has_d = true; else if (isalpha((unsigned char)core[i])) has_a = true; }
    if (has_d && has_a) {
        size_t i = 0;
        while (i < L) {
            size_t j = i; bool dig = isdigit((unsigned char)core[i]);
            if (dig) { while (j < L && isdigit((unsigned char)core[j])) j++; char t[64]; snprintf(t, sizeof t, "%.*s", (int)(j - i < 60 ? j - i : 60), core + i); Num n; if (parse_num(t, &n)) { if (n.ipn > 4 || (n.ipn > 1 && n.ip[0] == '0')) digits_words(out, n.ip, n.ipn, "zero"); else num_cardinal(out, &n, o, false); } }
            else { while (j < L && !isdigit((unsigned char)core[j])) j++; char t[128]; snprintf(t, sizeof t, "%.*s", (int)(j - i < 120 ? j - i : 120), core + i);
                   if (strlen(t) <= 3 && is_alpha_s(t) && strcasecmp(t, "win") && strcasecmp(t, "top") && strcasecmp(t, "max") && strcasecmp(t, "pro") && strcasecmp(t, "gen")) emit_spelled(out, t, strlen(t)); else { Ctx c2 = *c; norm_core(&c2, t, NULL, out); } }
            i = j;
        }
        return 0;
    }
    /* ---- camelCase / PascalCase / ALLCAPS+Word splitting ---- */
    if (L > 3 && is_alpha_s(core)) {
        bool split = false; for (size_t i = 1; i < L; i++) if ((islower((unsigned char)core[i - 1]) && isupper((unsigned char)core[i])) || (i + 1 < L && isupper((unsigned char)core[i - 1]) && isupper((unsigned char)core[i]) && islower((unsigned char)core[i + 1]) && i >= 2)) split = true;
        if (split) { size_t st = 0; for (size_t i = 1; i <= L; i++) {
                bool brk = i == L || (islower((unsigned char)core[i - 1]) && isupper((unsigned char)core[i])) || (i + 1 < L && isupper((unsigned char)core[i - 1]) && isupper((unsigned char)core[i]) && islower((unsigned char)core[i + 1]) && i - st >= 2);
                if (brk) { char t[128]; snprintf(t, sizeof t, "%.*s", (int)(i - st < 120 ? i - st : 120), core + st); tb_word(out, t); st = i; } }
            return 0; }
    }
    tb_word(out, core);
    return 0;
}

static bool is_lead_punct(char c) { return c == '(' || c == '[' || c == '{' || c == '"' || c == '\'' || c == '<' || c == '`' || c == '\x0e'; }
static bool is_trail_punct(char c) { return c == ')' || c == ']' || c == '}' || c == '"' || c == '\'' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '.' || c == '>' ; }

char *tts_normalize(const char *text, const TtsOpts *o) {
    char *src = clean_text(text);
    /* paragraph breaks: blank line -> \x03 */
    TBuf pre; tb_init(&pre);
    for (const char *p = src; *p; p++) {
        if (*p == '{') { const char *e = strchr(p, '}'); if (e) { for (; p <= e; p++) tb_putc(&pre, *p == ' ' ? '\x07' : *p); p--; continue; } }
        if (*p == '\n') { const char *q = p + 1; while (*q == ' ' || *q == '\t') q++; if (*q == '\n') { while (*q == '\n' || *q == ' ' || *q == '\t') q++; tb_puts(&pre, " \x03 "); p = q - 1; } else tb_putc(&pre, ' '); }
        else if (*p == '\t') tb_putc(&pre, ' '); else tb_putc(&pre, *p);
    }
    free(src);
    /* split tokens on spaces */
    char **tok = NULL; int nt = 0, ct = 0; char *save = pre.d ? pre.d : (char *)"";
    for (char *p = save; *p; ) {
        while (*p == ' ') p++; if (!*p) break;
        char *e = p; while (*e && *e != ' ') e++;
        if (nt == ct) { ct = ct ? ct * 2 : 64; tok = realloc(tok, (size_t)ct * sizeof(char *)); }
        tok[nt++] = tts_strndup(p, (size_t)(e - p)); p = e;
    }
    TBuf out; tb_init(&out); Ctx c; c.o = o; c.prevw[0] = 0; c.had_dot = false;
    for (int i = 0; i < nt; i++) {
        char *t = tok[i]; size_t L = strlen(t);
        if (t[0] == '{' && strchr(t, '}')) {      /* inline phoneme group: pass through untouched */
            if (out.n && out.d[out.n - 1] != ' ') tb_putc(&out, ' ');
            const char *cl = strchr(t, '}');
            for (const char *q = t; q <= cl; q++) tb_putc(&out, *q == '\x07' ? ' ' : *q);
            for (const char *q = cl + 1; *q; q++) if (strchr(",;:!?.", *q)) tb_putc(&out, *q);
            c.prevw[0] = 0; continue;
        }
        /* peel leading punctuation */
        size_t a = 0; while (a < L && is_lead_punct(t[a]) && !(a == 0 && t[a] == '<' && 0)) a++;
        size_t z = L;
        /* ellipsis and trailing punctuation; keep a final '.' that belongs to dotted abbreviations (U.S.A.) */
        while (z > a && is_trail_punct(t[z - 1])) z--;
        char lead[16], trail[32]; snprintf(lead, sizeof lead, "%.*s", (int)(a < 15 ? a : 15), t); snprintf(trail, sizeof trail, "%.*s", (int)((L - z) < 31 ? (L - z) : 31), t + z);
        char core[256]; snprintf(core, sizeof core, "%.*s", (int)((z - a) < 255 ? (z - a) : 255), t + a);
        /* dotted abbreviations keep their last period: "U.S.A." / "a.m." / "e.g." / "Ph.D." */
        if (trail[0] == '.' && strchr(core, '.') && strlen(core) >= 2 && isalpha((unsigned char)core[strlen(core) - 1])) { bool all = true; for (size_t k = 0; core[k]; k++) if (k % 2 ? core[k] != '.' : !isalpha((unsigned char)core[k])) all = false; if (all) { strcat(core, "."); memmove(trail, trail + 1, strlen(trail)); } }
        if (a && lead[0]) { for (size_t k = 0; lead[k]; k++) if (lead[k] == '(' || lead[k] == '[' || lead[k] == '{') tb_putc(&out, '(');  }
        const char *nxt = NULL; char ncore[128]; ncore[0] = 0;
        if (i + 1 < nt) { const char *q = tok[i + 1]; size_t ql = strlen(q), qa = 0; while (qa < ql && is_lead_punct(q[qa])) qa++; size_t qz = ql; while (qz > qa && is_trail_punct(q[qz - 1])) qz--; snprintf(ncore, sizeof ncore, "%.*s", (int)((qz - qa) < 127 ? (qz - qa) : 127), q + qa); nxt = ncore; if (!ncore[0]) nxt = NULL; }
        int consumed = 0;
        if (core[0]) {
            if (out.n && out.d[out.n - 1] != ' ' && out.d[out.n - 1] != '(') tb_putc(&out, ' ');
            /* abbreviation periods: "Dr." drops the period; a core ending in '.' keeps spelled form */
            size_t cl = strlen(core); bool dotend = cl && core[cl - 1] == '.'; bool had = dotend || trail[0] == '.'; if (dotend) core[cl - 1] = 0;
            if (nxt && trail[0] == '.' ) { char lowc[64]; size_t k = 0; for (; core[k] && k < 63; k++) lowc[k] = (char)tolower((unsigned char)core[k]); lowc[k] = 0; for (const Abbr *ab = ABBR; ab->ab; ab++) if (!strcmp(ab->ab, lowc) && ab->drop_dot) { trail[0] = ' '; memmove(trail, trail + 1, strlen(trail)); break; } }
            if (trail[0] == '.' && !nxt) {}
            c.had_dot = had;
            consumed = norm_core(&c, core, nxt, &out);
            snprintf(c.prevw, sizeof c.prevw, "%s", core); for (char *q = c.prevw; *q; q++) *q = (char)tolower((unsigned char)*q);
        }
        for (size_t k = 0; trail[k]; k++) { char ch = trail[k]; if (ch == '\'' || ch == '"' || ch == ')' || ch == ']' || ch == '}' || ch == '>') { if (ch == ')' || ch == ']' || ch == '}') tb_putc(&out, ')'); continue; } tb_putc(&out, ch); }
        if (trail[0]) c.prevw[0] = trail[0] == '.' ? '.' : 0;
        if (consumed) i += consumed;
    }
    for (int i = 0; i < nt; i++) free(tok[i]);
    free(tok); tb_free(&pre);
    return tb_take(&out);
}

/* ================================================================== words */
void tts_words_free(TtsWords *ws) { for (int i = 0; i < ws->n; i++) { free(ws->w[i].text); free(ws->w[i].ph); } free(ws->w); free(ws->sty); ws->w = NULL; ws->sty = NULL; ws->n = ws->cap = ws->nsty = ws->capsty = 0; }
TtsWord *tts_words_push(TtsWords *ws, int kind, const char *text) {
    if (ws->n == ws->cap) { ws->cap = ws->cap ? ws->cap * 2 : 64; ws->w = realloc(ws->w, (size_t)ws->cap * sizeof(TtsWord)); }
    TtsWord *w = &ws->w[ws->n++]; memset(w, 0, sizeof *w);
    w->kind = (uint8_t)kind; w->text = strdup(text ? text : ""); w->rate = 1; w->vol = 1; w->voice = -1;
    return w;
}

/* span attributes (SSML) */
typedef struct { float rate, pitch, vol; int voice, emph; bool slang, caps_emph; } Attr;
static bool is_word_char(char c) { return isalnum((unsigned char)c) || c == '\''; }

static void push_punct(TtsWords *ws, char ch, int para) {
    if (ws->n && ws->w[ws->n - 1].kind == WK_PUNCT) {
        TtsWord *pw = &ws->w[ws->n - 1];
        bool pe = pw->punc == '.' || pw->punc == '?' || pw->punc == '!' || pw->punc == '~';
        bool ce = ch == '.' || ch == '?' || ch == '!' || ch == '~';
        if (para) pw->para = 1;
        if (ce && !pe) { pw->punc = ch; pw->text[0] = ch; return; }
        if (ce && pe) { if (ch == '?' || ch == '!' || (ch == '~' && pw->punc == '.')) { pw->punc = ch; pw->text[0] = ch; } return; }
        if (!ce && !pe && ch == ',') return;
        if (pe) return;
        if (ch == ':' || ch == ';') { pw->punc = ch; pw->text[0] = ch; }
        return;
    }
    if (!ws->n) return;       /* leading punctuation carries no pause */
    char t[2] = { ch, 0 }; TtsWord *w = tts_words_push(ws, WK_PUNCT, t); w->punc = ch; w->para = para; w->src = SRC_PUNCT;
}

/* tokenise one normalised span */
static void tokenise_span(const char *s, const Attr *at, TtsWords *ws, KcPos pos, int *emph_state) {
    for (const char *p = s; *p; ) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t' || c == '\n') { p++; continue; }
        if (c == '\x05') { *emph_state = 2; p++; continue; }
        if (c == '\x06') { *emph_state = 0; p++; continue; }
        if (c == '\x03') { push_punct(ws, '.', 1); p++; continue; }
        if (c == '\x01') {
            const char *e = strchr(p + 1, '\x02'); if (!e) e = p + strlen(p);
            for (const char *q = p + 1; q < e; q++) if (isalpha((unsigned char)*q)) {
                char t[2] = { (char)tolower((unsigned char)*q), 0 };
                TtsWord *w = tts_words_push(ws, WK_WORD, t); w->src = SRC_SPELL; w->content = 1; w->caps = 1; w->rate = at->rate; w->pitch_st = at->pitch; w->vol = at->vol; w->voice = at->voice; w->emph = (uint8_t)(*emph_state ? *emph_state : at->emph);
                TtsPh tmp[16]; int n; tts_spell_word(t, tmp, &n, 16); w->ph = malloc((size_t)(n ? n : 1) * sizeof(TtsPh)); memcpy(w->ph, tmp, (size_t)n * sizeof(TtsPh)); w->nph = n;
            }
            p = *e ? e + 1 : e; continue;
        }
        if (c == '{') {
            const char *e = strchr(p, '}'); if (!e) kc_error_at(pos, "tts: unterminated '{' phoneme group");
            char buf[256]; size_t L = (size_t)(e - p - 1); if (L >= sizeof buf) L = sizeof buf - 1; memcpy(buf, p + 1, L); buf[L] = 0;
            TtsPh ph[96]; int n; bool ascii = true; for (const char *q = buf; *q; q++) if ((unsigned char)*q >= 0x80) ascii = false;
            char bad[32] = ""; n = ascii ? tts_phseq_parse(buf, ph, 96, bad, sizeof bad) : tts_ipa_parse(buf, ph, 96);
            if (n < 0) kc_error_at(pos, "tts: unknown phoneme '%s' in {%s} (use ARPAbet like {HH AH0 L OW1} or IPA)", ascii ? bad : buf, buf);
            TtsWord *w = tts_words_push(ws, WK_PHONES, buf); w->src = SRC_PHONES; w->content = 1; w->rate = at->rate; w->pitch_st = at->pitch; w->vol = at->vol; w->voice = at->voice; w->emph = (uint8_t)(*emph_state ? *emph_state : at->emph);
            w->ph = malloc((size_t)(n ? n : 1) * sizeof(TtsPh)); memcpy(w->ph, ph, (size_t)n * sizeof(TtsPh)); w->nph = n;
            p = e + 1; continue;
        }
        if (isalpha(c) || isdigit(c) || (c == '\'' && is_word_char(p[1]) && isalpha((unsigned char)p[1]) && ws->n && 0)) {
            char buf[128]; int k = 0; bool caps = true; int letters = 0;
            while (is_word_char(*p) || (*p == '-' && isalpha((unsigned char)p[1]) && k > 0 && isalpha((unsigned char)p[-1]) && 0)) { if (k < 126) { buf[k++] = *p; } if (isupper((unsigned char)*p)) letters++; else if (islower((unsigned char)*p)) caps = false; p++; }
            while (k && buf[k - 1] == '\'') k--;
            buf[k] = 0; if (!k) continue;
            bool lead_cap = false; (void)lead_cap;
            int nl = 0; for (int i = 0; i < k; i++) if (isalpha((unsigned char)buf[i])) nl++;
            for (int i = 0; i < k; i++) buf[i] = (char)tolower((unsigned char)buf[i]);
            /* elongation: sooo / noooo / pleeease -> collapsed word, slowed down */
            float stretch = 1.0f;
            { char t2[128]; int m = 0; bool did = false;
              for (int i = 0; i < k && m < 126; ) {
                  int j = i; while (j < k && buf[j] == buf[i]) j++;
                  int run = j - i;
                  if (run >= 3 && isalpha((unsigned char)buf[i])) { t2[m++] = buf[i]; did = true; float st = 1.0f + 0.3f * (float)(run - 2); if (st > stretch) stretch = st; }
                  else for (int q = 0; q < run && m < 126; q++) t2[m++] = buf[i];
                  i = j;
              }
              if (did && m >= 2) { memcpy(buf, t2, (size_t)m); buf[m] = 0; k = m; if (stretch > 2.2f) stretch = 2.2f; } else stretch = 1.0f; }
            if (at->slang) {
                static const struct { const char *a, *b; } SL[] = { { "u", "you" }, { "ur", "your" }, { "r", "are" }, { "pls", "please" }, { "plz", "please" }, { "thx", "thanks" }, { "thnx", "thanks" }, { "ty", "thank you" },
                    { "tho", "though" }, { "cuz", "because" }, { "cos", "because" }, { "coz", "because" }, { "bc", "because" }, { "cmon", "come on" }, { "idk", "I don't know" }, { "imo", "in my opinion" }, { "omg", "oh my god" },
                    { "brb", "be right back" }, { "gtg", "got to go" }, { "btw", "by the way" }, { "tbh", "to be honest" }, { "rn", "right now" }, { "ily", "I love you" }, { "b4", "before" }, { "gr8", "great" },
                    { "2day", "today" }, { "2morrow", "tomorrow" }, { "2nite", "tonight" }, { "l8r", "later" }, { "m8", "mate" }, { "np", "no problem" }, { "jk", "just kidding" }, { "smh", "shaking my head" },
                    { "dm", "D M" }, { "ppl", "people" }, { "abt", "about" }, { "w/", "with" }, { "wut", "what" }, { "wat", "what" }, { "yea", "yeah" }, { "k", "okay" }, { "kk", "okay" }, { "ok", "okay" } };
                const char *rep = NULL; for (size_t q = 0; q < sizeof SL / sizeof SL[0]; q++) if (!strcmp(buf, SL[q].a)) { rep = SL[q].b; break; }
                if (rep && strcmp(buf, "ok")) { TBuf sb; tb_init(&sb); tb_puts(&sb, rep); int es = *emph_state; tokenise_span(sb.d, at, ws, pos, &es); tb_free(&sb); continue; }
            }
            const char *sub = tts_sub_lookup(buf);
            if (sub) {
                TBuf sb; tb_init(&sb); tb_puts(&sb, sub);
                int es = *emph_state; tokenise_span(sb.d, at, ws, pos, &es); tb_free(&sb);
                continue;
            }
            /* digits left over (should have been expanded) -> read them */
            bool alld = true; for (int i = 0; i < k; i++) if (!isdigit((unsigned char)buf[i])) alld = false;
            if (alld) { TBuf t; tb_init(&t); static const TtsOpts dflt = {0}; (void)dflt; for (int i = 0; i < k; i++) tb_word(&t, ONES[buf[i] - '0']); int es = *emph_state; tokenise_span(t.d, at, ws, pos, &es); tb_free(&t); continue; }
            TtsWord *w = tts_words_push(ws, WK_WORD, buf);
            w->caps = (caps && nl >= 2) ? 1 : 0; w->rate = at->rate; w->pitch_st = at->pitch; w->vol = at->vol; w->voice = at->voice; w->emph = (uint8_t)(*emph_state ? *emph_state : at->emph);
            if (stretch > 1.0f) { w->rate /= stretch; if (!w->emph) w->emph = 1; }
            if (at->caps_emph && w->caps && k >= 3) w->emph = 2;
            continue;
        }
        /* punctuation */
        if (c == '.' && p[1] == '.' && p[2] == '.') { push_punct(ws, '~', 0); p += 3; while (*p == '.') p++; continue; }
        switch (c) {
        case '.': case '?': case '!': case ',': case ';': case ':': push_punct(ws, (char)c, 0); break;
        case '-': push_punct(ws, ',', 0); break;
        case '(': case ')': push_punct(ws, ',', 0); break;
        default: break;
        }
        p++;
    }
}

/* ---- SSML subset ---- */
static bool attr_get(const char *tag, const char *name, char *out, size_t n) {
    const char *p = tag; size_t nl = strlen(name);
    while ((p = strstr(p, name))) {
        if ((p == tag || isspace((unsigned char)p[-1])) ) {
            const char *q = p + nl; while (*q == ' ') q++;
            if (*q == '=') { q++; while (*q == ' ') q++; char qc = *q; if (qc == '"' || qc == '\'') { q++; const char *e = strchr(q, qc); if (!e) return false; size_t L = (size_t)(e - q); if (L >= n) L = n - 1; memcpy(out, q, L); out[L] = 0; return true; } }
        }
        p += nl;
    }
    return false;
}
static void ent_decode(const char *in, TBuf *b) {
    for (const char *p = in; *p; p++) {
        if (*p == '&') { if (!strncmp(p, "&amp;", 5)) { tb_putc(b, '&'); p += 4; continue; } if (!strncmp(p, "&lt;", 4)) { tb_putc(b, '<'); p += 3; continue; } if (!strncmp(p, "&gt;", 4)) { tb_putc(b, '>'); p += 3; continue; } if (!strncmp(p, "&quot;", 6)) { tb_putc(b, '"'); p += 5; continue; } if (!strncmp(p, "&apos;", 6)) { tb_putc(b, '\''); p += 5; continue; } }
        tb_putc(b, *p);
    }
}
static double parse_time_ms(const char *s) { char *e; double v = strtod(s, &e); if (e == s) return 0; if (!strncmp(e, "ms", 2)) return v; if (*e == 's') return v * 1000; return v; }
static float parse_rate(const char *s) {
    if (!strcmp(s, "x-slow")) return 0.5f; if (!strcmp(s, "slow")) return 0.75f; if (!strcmp(s, "medium")) return 1.0f; if (!strcmp(s, "fast")) return 1.3f; if (!strcmp(s, "x-fast")) return 1.7f;
    char *e; double v = strtod(s, &e); if (e == s) return 1.0f; if (*e == '%') { return (float)((s[0] == '+' || s[0] == '-') ? 1.0 + v / 100.0 : v / 100.0); } return (float)v;
}
static float parse_pitch_st(const char *s) {
    if (!strcmp(s, "x-low")) return -6; if (!strcmp(s, "low")) return -3; if (!strcmp(s, "medium")) return 0; if (!strcmp(s, "high")) return 3; if (!strcmp(s, "x-high")) return 6;
    char *e; double v = strtod(s, &e); if (e == s) return 0; if (!strncmp(e, "st", 2)) return (float)v; if (*e == '%') return (float)(12.0 * log2(1.0 + v / 100.0)); return 0;
}
static float parse_vol(const char *s) {
    if (!strcmp(s, "silent")) return 0; if (!strcmp(s, "x-soft")) return 0.3f; if (!strcmp(s, "soft")) return 0.6f; if (!strcmp(s, "medium")) return 1; if (!strcmp(s, "loud")) return 1.4f; if (!strcmp(s, "x-loud")) return 1.8f;
    char *e; double v = strtod(s, &e); if (e == s) return 1; if (!strncmp(e, "dB", 2)) return (float)pow(10.0, v / 20.0); return (float)v;
}

/* ---- style state ([happy] ... [/happy]) ---- */
typedef struct { int style[8]; float amount[8]; int ncomp; int ofield[8]; float oval[8]; int nov; int voice; int cur; } SState;
int tts_words_style(TtsWords *ws, const TtsStyle *st) {
    if (!ws->nsty) { ws->capsty = 8; ws->sty = malloc((size_t)ws->capsty * sizeof(TtsStyle)); tts_style_neutral(&ws->sty[0]); ws->nsty = 1; }
    for (int i = 0; i < ws->nsty; i++) if (!memcmp(&ws->sty[i], st, sizeof *st)) return i;
    if (ws->nsty == ws->capsty) { ws->capsty *= 2; ws->sty = realloc(ws->sty, (size_t)ws->capsty * sizeof(TtsStyle)); }
    ws->sty[ws->nsty] = *st; return ws->nsty++;
}
static int ss_resolve_with(const SState *ss, TtsWords *ws, int extra_style, float extra_amount) {
    const TtsStyle *st[9]; float am[9]; int n = 0;
    for (int i = 0; i < ss->ncomp; i++) { st[n] = tts_style_at(ss->style[i]); am[n++] = ss->amount[i]; }
    if (extra_style > 0 && n < 9) { st[n] = tts_style_at(extra_style); am[n++] = extra_amount; }
    TtsStyle out; tts_style_compose(&out, st, am, n);
    for (int i = 0; i < ss->nov; i++) tts_style_set_field(&out, ss->ofield[i], ss->oval[i]);
    return tts_words_style(ws, &out);
}
static void ss_init(SState *ss, const TtsOpts *o, TtsWords *ws, KcPos pos) {
    memset(ss, 0, sizeof *ss); ss->voice = -1;
    if (o->style[0]) {
        int si = tts_style_find(o->style);
        if (si < 0) { const char *c = tts_style_canon(o->style); kc_error_at(pos, "tts: unknown style '%s'%s%s (see tts.styles())", o->style, c ? " — did you mean " : "", c ? c : ""); }
        if (si > 0) { ss->style[0] = si; ss->amount[0] = (float)(o->style_amount > 0 ? o->style_amount : 1.0); ss->ncomp = 1; }
    }
    ss->cur = ss_resolve_with(ss, ws, 0, 0);
}
static void stamp(TtsWords *ws, int from, const SState *ss) {
    for (int i = from; i < ws->n; i++) { ws->w[i].style = ss->cur; if (ws->w[i].voice < 0 && ss->voice >= 0) ws->w[i].voice = ss->voice; }
}
static void apply_tag(const TagParse *tp, const char *inner, SState *ss, const Attr *at, TtsWords *ws, KcPos pos) {
    bool style_set = false;
    for (int i = 0; i < tp->n; i++) {
        const TagOp *op = &tp->op[i];
        switch (op->type) {
        case TOP_STYLE:
            if (tp->closing) { int k = 0; for (int j = 0; j < ss->ncomp; j++) if (ss->style[j] != op->idx) { ss->style[k] = ss->style[j]; ss->amount[k++] = ss->amount[j]; } ss->ncomp = k; }
            else { if (!tp->add && !style_set) { ss->ncomp = 0; ss->nov = 0; } style_set = true; if (ss->ncomp < 8) { ss->style[ss->ncomp] = op->idx; ss->amount[ss->ncomp++] = op->amount; } }
            break;
        case TOP_RESET: ss->ncomp = 0; ss->nov = 0; ss->voice = -1; break;
        case TOP_VOICE:
            if (tp->closing) { ss->voice = -1; break; }
            { int vi = tts_voice_find(op->name); if (vi < 0) kc_error_at(pos, "tts: unknown voice '%s' in tag [%s]", op->name, inner); ss->voice = vi; }
            break;
        case TOP_PARAM: {
            int j = 0; for (; j < ss->nov; j++) if (ss->ofield[j] == op->field) break;
            if (j == ss->nov && ss->nov < 8) ss->nov++;
            if (j < 8) { ss->ofield[j] = op->field; ss->oval[j] = op->value; }
            break; }
        default: break;
        }
    }
    ss->cur = ss_resolve_with(ss, ws, 0, 0);
    TtsWord *sw = tts_words_push(ws, WK_STYLE, inner); sw->style = ss->cur; sw->voice = ss->voice;
    for (int i = 0; i < tp->n; i++) {
        const TagOp *op = &tp->op[i];
        if (op->type == TOP_PAUSE) { TtsWord *w = tts_words_push(ws, WK_BREAK, ""); w->brk_ms = op->value; w->style = ss->cur; }
        else if (op->type == TOP_EVENT && !tp->closing) { TtsWord *w = tts_words_push(ws, WK_EVENT, op->name); w->event = op->idx; w->amount = op->amount; w->style = ss->cur; w->voice = ss->voice >= 0 ? ss->voice : at->voice; w->vol = at->vol; w->pitch_st = at->pitch; }
        else if (op->type == TOP_INTERJ && !tp->closing) {
            float pitch, stretch; const char *mood; const char *ph = tts_interj_phones(op->idx, &pitch, &stretch, &mood);
            TtsPh pp[16]; int n = tts_phseq_parse(ph, pp, 16, NULL, 0); if (n <= 0) continue;
            TtsWord *w = tts_words_push(ws, WK_PHONES, op->name); w->src = SRC_PHONES; w->content = 1; w->emph = 1;
            w->pitch_st = at->pitch + pitch * (op->amount > 1 ? 1.0f : op->amount); w->rate = at->rate / (stretch * (op->amount > 1 ? op->amount : 1.0f)); w->vol = at->vol; w->voice = ss->voice >= 0 ? ss->voice : at->voice;
            int mi = (mood && *mood) ? tts_style_find(mood) : 0; w->style = ss_resolve_with(ss, ws, mi > 0 ? mi : 0, 0.6f);
            w->ph = malloc((size_t)n * sizeof(TtsPh)); memcpy(w->ph, pp, (size_t)n * sizeof(TtsPh)); w->nph = n;
        }
    }
}
static char *say_as_text(const char *text, const char *mode, const TtsOpts *o);
static bool tag_like(const char *s) { int a = 0, n = 0; for (; *s; s++, n++) if (isalpha((unsigned char)*s)) a++; return a >= 3 && n <= 32; }
/* raw text -> tokens. Splits at [tags], normalises each plain piece, stamps the active style on its words. */
static void tokenise_text(const char *raw, const TtsOpts *o, const Attr *at, SState *ss, const char *say, TtsWords *ws, KcPos pos, int *emph_state) {
    const char *seg = raw;
    #define FLUSH_PIECE(END) do { size_t pl_ = (size_t)((END) - seg); if (pl_) { char *piece = tts_strndup(seg, pl_); bool blank = true; for (const char *q_ = piece; *q_; q_++) if (!isspace((unsigned char)*q_)) blank = false; \
        if (!blank) { char *nm = (say && say[0]) ? say_as_text(piece, say, o) : (o->normalize ? tts_normalize(piece, o) : clean_text(piece)); int from = ws->n; tokenise_span(nm, at, ws, pos, emph_state); stamp(ws, from, ss); free(nm); } free(piece); } } while (0)
    for (const char *p = raw; *p; ) {
        if (o->tags && *p == '[') {
            const char *e = p + 1; while (*e && *e != ']' && *e != '[' && (e - p) < 100) e++;
            if (*e == ']') {
                char inner[104]; size_t L = (size_t)(e - p - 1); memcpy(inner, p + 1, L); inner[L] = 0;
                TagParse tp; char sug[32];
                if (tts_tag_parse(inner, &tp, sug, sizeof sug)) { FLUSH_PIECE(p); apply_tag(&tp, inner, ss, at, ws, pos); p = e + 1; seg = p; continue; }
                if (o->strict_tags && tag_like(inner)) kc_error_at(pos, "tts: unknown style tag [%s]%s%s%s", inner, sug[0] ? " — did you mean [" : "", sug, sug[0] ? "]?" : "");
            }
        }
        p++;
    }
    FLUSH_PIECE(raw + strlen(raw));
    #undef FLUSH_PIECE
}

typedef struct { char name[24]; Attr at; char say[32]; SState ss; } Frame;
static char *say_as_text(const char *text, const char *mode, const TtsOpts *o) {
    if (!strcmp(mode, "characters") || !strcmp(mode, "spell-out") || !strcmp(mode, "verbatim") || !strcmp(mode, "letters")) { TBuf b; tb_init(&b); tb_puts(&b, "\x01"); for (const char *p = text; *p; p++) if (isalnum((unsigned char)*p)) { if (isdigit((unsigned char)*p)) { tb_putc(&b, '\x02'); tb_word(&b, ONES[*p - '0']); tb_puts(&b, " \x01"); } else tb_putc(&b, *p); } tb_putc(&b, '\x02'); return tb_take(&b); }
    TBuf b; tb_init(&b); Num n; const char *p = text; while (*p == ' ') p++;
    if (!strcmp(mode, "digits") || !strcmp(mode, "telephone")) { digits_words(&b, p, (int)strlen(p), !strcmp(mode, "telephone") ? "oh" : "zero"); return tb_take(&b); }
    if (parse_num(p, &n) && !n.has_dec) {
        if (!strcmp(mode, "ordinal")) { tts_ordinal_ll(&b, strtoll(n.ip, NULL, 10), o && o->and_word); return tb_take(&b); }
        if (!strcmp(mode, "cardinal") || !strcmp(mode, "number")) { if (n.neg) tb_word(&b, "minus"); tts_cardinal_ll(&b, strtoll(n.ip, NULL, 10), o && o->and_word); return tb_take(&b); }
        if (!strcmp(mode, "year")) { tts_year_words(&b, atoi(n.ip)); return tb_take(&b); }
    }
    tb_free(&b); return tts_normalize(text, o);
}

static void ssml_parse(const char *s, const TtsOpts *o, TtsWords *ws, KcPos pos, const SState *init, bool has_lower) {
    Frame stack[32]; int sp = 0; memset(stack, 0, sizeof stack);
    stack[0].at.rate = 1; stack[0].at.vol = 1; stack[0].at.voice = -1; stack[0].at.emph = 0; stack[0].at.slang = o->slang; stack[0].at.caps_emph = o->caps_emph && has_lower; stack[0].ss = *init;
    int emph_state = 0; TBuf text; tb_init(&text);
    #define FLUSH() do { if (text.n) { TBuf d; tb_init(&d); ent_decode(text.d, &d); tokenise_text(d.d, o, &stack[sp].at, &stack[sp].ss, stack[sp].say, ws, pos, &emph_state); tb_free(&d); text.n = 0; text.d[0] = 0; } } while (0)
    for (const char *p = s; *p; ) {
        if (*p != '<') { tb_putc(&text, *p++); continue; }
        if (!strncmp(p, "<!--", 4)) { const char *e = strstr(p, "-->"); p = e ? e + 3 : p + strlen(p); continue; }
        const char *e = strchr(p, '>'); if (!e) kc_error_at(pos, "tts: unterminated SSML tag");
        char tag[512]; size_t L = (size_t)(e - p - 1); if (L >= sizeof tag) L = sizeof tag - 1; memcpy(tag, p + 1, L); tag[L] = 0; p = e + 1;
        bool closing = tag[0] == '/', selfc = L && tag[L - 1] == '/';
        char name[24]; int k = 0; const char *q = tag + (closing ? 1 : 0); while (*q && !isspace((unsigned char)*q) && *q != '/' && k < 23) name[k++] = (char)tolower((unsigned char)*q++); name[k] = 0;
        char v[256];
        FLUSH();
        if (closing) {
            if (!strcmp(name, "p") || !strcmp(name, "s")) push_punct(ws, '.', !strcmp(name, "p"));
            if (!strcmp(name, "emphasis")) emph_state = 0;
            if (sp > 0 && !strcmp(stack[sp].name, name)) sp--;
            continue;
        }
        if (!strcmp(name, "break")) {
            double ms = 0;
            if (attr_get(tag, "time", v, sizeof v)) ms = parse_time_ms(v);
            else if (attr_get(tag, "strength", v, sizeof v)) ms = !strcmp(v, "none") ? 0 : !strcmp(v, "x-weak") ? 100 : !strcmp(v, "weak") ? 200 : !strcmp(v, "medium") ? 400 : !strcmp(v, "strong") ? 700 : 1000;
            else ms = 400;
            TtsWord *w = tts_words_push(ws, WK_BREAK, ""); w->brk_ms = (float)ms; continue;
        }
        if (!strcmp(name, "mark")) { if (attr_get(tag, "name", v, sizeof v)) tts_words_push(ws, WK_MARK, v); continue; }
        if (!strcmp(name, "sub") && !selfc) {
            char alias[256] = ""; attr_get(tag, "alias", alias, sizeof alias);
            const char *close = strstr(p, "</sub>"); p = close ? close + 6 : p + strlen(p);
            char *nm = tts_normalize(alias, o); tokenise_span(nm, &stack[sp].at, ws, pos, &emph_state); free(nm); continue;
        }
        if (!strcmp(name, "phoneme") && !selfc) {
            char ph[256] = "", alpha[16] = "ipa"; attr_get(tag, "ph", ph, sizeof ph); attr_get(tag, "alphabet", alpha, sizeof alpha);
            const char *close = strstr(p, "</phoneme>"); size_t il = close ? (size_t)(close - p) : strlen(p); char inner[128]; snprintf(inner, sizeof inner, "%.*s", (int)(il < 127 ? il : 127), p); p = close ? close + 10 : p + strlen(p);
            TtsPh pp[96]; int n; char bad[32] = "";
            n = !strcasecmp(alpha, "ipa") ? tts_ipa_parse(ph, pp, 96) : tts_phseq_parse(ph, pp, 96, bad, sizeof bad);
            if (n < 0) kc_error_at(pos, "tts: bad <phoneme ph=\"%s\"> (alphabet %s)", ph, alpha);
            TtsWord *w = tts_words_push(ws, WK_PHONES, inner); w->src = SRC_PHONES; w->content = 1; w->rate = stack[sp].at.rate; w->pitch_st = stack[sp].at.pitch; w->vol = stack[sp].at.vol; w->voice = stack[sp].at.voice;
            w->ph = malloc((size_t)(n ? n : 1) * sizeof(TtsPh)); memcpy(w->ph, pp, (size_t)n * sizeof(TtsPh)); w->nph = n; continue;
        }
        if (selfc) continue;
        if (sp >= 31) kc_error_at(pos, "tts: SSML nesting too deep");
        Frame f = stack[sp]; snprintf(f.name, sizeof f.name, "%.23s", name);
        if (!strcmp(name, "prosody")) {
            if (attr_get(tag, "rate", v, sizeof v)) f.at.rate *= parse_rate(v);
            if (attr_get(tag, "pitch", v, sizeof v)) f.at.pitch += parse_pitch_st(v);
            if (attr_get(tag, "volume", v, sizeof v)) f.at.vol *= parse_vol(v);
        } else if (!strcmp(name, "emphasis")) {
            f.at.emph = 2; if (attr_get(tag, "level", v, sizeof v)) { if (!strcmp(v, "reduced") || !strcmp(v, "none")) f.at.emph = 0; else if (!strcmp(v, "moderate")) f.at.emph = 1; }
            emph_state = f.at.emph;
        } else if (!strcmp(name, "voice")) {
            if (attr_get(tag, "name", v, sizeof v)) { int vi = tts_voice_find(v); if (vi < 0) kc_error_at(pos, "tts: unknown voice '%s' in <voice>", v); f.at.voice = vi; }
        } else if (!strcmp(name, "style")) {
            char sn[96] = ""; attr_get(tag, "name", sn, sizeof sn); char am[16] = ""; if (attr_get(tag, "amount", am, sizeof am) || attr_get(tag, "intensity", am, sizeof am)) { size_t L = strlen(sn); snprintf(sn + L, sizeof sn - L, " %s", am); }
            TagParse tp; char sug[32]; if (!tts_tag_parse(sn, &tp, sug, sizeof sug)) kc_error_at(pos, "tts: unknown style '%s' in <style>%s%s", sn, sug[0] ? " — did you mean " : "", sug);
            apply_tag(&tp, sn, &f.ss, &f.at, ws, pos);
        } else if (!strcmp(name, "say-as")) {
            f.say[0] = 0; if (attr_get(tag, "interpret-as", v, sizeof v)) snprintf(f.say, sizeof f.say, "%.31s", v);
        }
        stack[++sp] = f;
    }
    FLUSH();
    #undef FLUSH
    tb_free(&text);
}

static bool acronym_word(const char *w) {
    static const char *const A[] = { "nasa", "nato", "ascii", "gui", "ram", "rom", "lan", "wan", "wifi", "sql", "regex", "gif", "jpeg", "json", "nginx", "unix", NULL };
    for (int i = 0; A[i]; i++) if (!strcmp(w, A[i])) return true;
    return false;
}
void tts_tokenize(const char *text, const TtsOpts *o, TtsWords *out, KcPos pos) {
    tts_g2p_init();
    const char *t = text; while (*t == ' ' || *t == '\n' || *t == '\t' || *t == '\r') t++;
    bool ssml = o->ssml || (o->ssml_auto && !strncmp(t, "<speak", 6));
    bool has_lower = false; for (const char *q = text; *q; q++) if (islower((unsigned char)*q)) { has_lower = true; break; }
    int emph_state = 0;
    SState ss; ss_init(&ss, o, out, pos);
    if (ssml) ssml_parse(text, o, out, pos, &ss, has_lower);
    else {
        Attr at = { 1, 0, 1, -1, 0, o->slang, o->caps_emph && has_lower };
        tokenise_text(text, o, &at, &ss, "", out, pos, &emph_state);
    }
    for (int i = 0; i < out->n; i++) {
        TtsWord *w = &out->w[i];
        if (w->kind != WK_WORD || w->nph > 0) continue;
        const char *prev = NULL, *next = NULL;
        for (int j = i - 1; j >= 0; j--) { if (out->w[j].kind == WK_WORD) { prev = out->w[j].text; break; } if (out->w[j].kind == WK_PUNCT && out->w[j].punc != ',') break; }
        for (int j = i + 1; j < out->n; j++) { if (out->w[j].kind == WK_WORD) { next = out->w[j].text; break; } if (out->w[j].kind == WK_PUNCT) break; }
        tts_g2p_word(w, prev, next, o);
        if (w->src == SRC_ACRONYM || acronym_word(w->text)) { if (w->caps) w->emph = 0; }
        w->content = (uint8_t)(!tts_is_function_word(w->text) || w->src == SRC_ACRONYM);
        if (w->emph) w->content = 1;
    }
}
