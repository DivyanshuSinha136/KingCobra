/* std.ascii_screen — retained-mode HUD / TUI widgets.
 * Kinds: text, panel, bar (health/mana/progress, optional damage trail), list (menu), button, input (text field),
 *        hotbar, graph (braille line chart), minimap (any canvas), spinner, crosshair, fps.
 * Plus: toasts (hud_notify), typewriter dialogs with choices, a debug overlay, tweens, focus + mouse + keyboard routing. */
#include "as_internal.h"

enum { H_TEXT, H_PANEL, H_BAR, H_LIST, H_BUTTON, H_INPUT, H_HOTBAR, H_GRAPH, H_MINIMAP, H_SPINNER, H_CROSSHAIR, H_FPS };
static const char *KIND_NAMES[] = { "text", "panel", "bar", "list", "button", "input", "hotbar", "graph", "minimap", "spinner", "crosshair", "fps", NULL };
typedef struct { int prop; double from, to, t, dur; int ease; bool active; } Tween;
typedef struct {
    bool used, visible; int kind, z, anchor, x, y, w, h;
    char *text, *title, box[16]; Col fg, bg, bfg, fill, empty, trail_c; uint16_t attr; int align;
    double value, max, shown; bool trail, vertical, hover;
    char **items; int nitems, sel, scroll;
    bool has_click, has_select; KcValue on_click, on_select;
    float *series; int ns; double smin, smax; int canvas; int cursor; Tween tw[4]; double t0;
} Hud;
static Hud *HU; static int nhu, caphu; static int focus_id = 0; static bool hud_on = true, dbg = false;
typedef struct { char *text; Col fg; float life, total; int anchor; } Toast;
static Toast toasts[8]; static int ntoast;
static struct { bool active; char *speaker, *text; char **choices; int nchoices, sel, result; float shown, speed; bool done; Col fg; } DLG;

void hud_set_enabled(bool on) { hud_on = on; }
void hud_set_debug(bool on) { dbg = on; }
static Hud *hu_get(int id) { return (id < 1 || id >= nhu || !HU[id].used) ? NULL : &HU[id]; }
static void free_items(Hud *h) { for (int i = 0; i < h->nitems; i++) free(h->items[i]); free(h->items); h->items = NULL; h->nitems = 0; }
static char *dupstr(const char *s) { char *d = kc_malloc(strlen(s) + 1); strcpy(d, s); return d; }
void hud_reset(void) {
    for (int i = 1; i < nhu; i++) if (HU[i].used) { free(HU[i].text); free(HU[i].title); free_items(&HU[i]); free(HU[i].series); HU[i].used = false; }
    nhu = 0; focus_id = 0; ntoast = 0; DLG.active = false;
}
static int anchor_parse(const char *s) {
    static const struct { const char *n; int v; } A[] = { { "tl", 0 }, { "top-left", 0 }, { "t", 1 }, { "top", 1 }, { "tc", 1 }, { "tr", 2 }, { "top-right", 2 }, { "l", 3 }, { "left", 3 }, { "c", 4 }, { "center", 4 }, { "r", 5 }, { "right", 5 },
        { "bl", 6 }, { "bottom-left", 6 }, { "b", 7 }, { "bottom", 7 }, { "bc", 7 }, { "br", 8 }, { "bottom-right", 8 } };
    for (size_t i = 0; i < sizeof A / sizeof A[0]; i++) if (!strcmp(A[i].n, s)) return A[i].v;
    as_err("hud: unknown anchor \"%s\" (tl, t, tr, l, c, r, bl, b, br)", s);
}
static char *val_text(KcValue v) { if (v.type == VAL_STRING) return dupstr(v.as.s->data); char *d = kc_to_display_string(v); char *r = dupstr(d); free(d); return r; }
static void apply_opts(Hud *h, const Opts *o) {
    const KcValue *v;
    if ((v = opt_find(o, "text"))) { free(h->text); h->text = val_text(*v); }
    if ((v = opt_find(o, "title"))) { free(h->title); h->title = val_text(*v); }
    if ((v = opt_find(o, "anchor"))) { if (v->type != VAL_STRING) as_err("hud: anchor must be a string"); h->anchor = anchor_parse(v->as.s->data); }
    if (opt_has(o, "x")) h->x = opt_int(o, "x", 0); if (opt_has(o, "y")) h->y = opt_int(o, "y", 0); if (opt_has(o, "w")) h->w = opt_int(o, "w", 0); if (opt_has(o, "h")) h->h = opt_int(o, "h", 0);
    if (opt_has(o, "z")) h->z = opt_int(o, "z", 0); if (opt_has(o, "visible")) h->visible = opt_bool(o, "visible", true);
    if (opt_has(o, "fg")) h->fg = opt_col(o, "fg", 0); if (opt_has(o, "bg")) h->bg = opt_col(o, "bg", 0); if (opt_has(o, "border_fg")) h->bfg = opt_col(o, "border_fg", 0);
    if (opt_has(o, "fill")) h->fill = opt_col(o, "fill", 0); if (opt_has(o, "empty")) h->empty = opt_col(o, "empty", 0); if (opt_has(o, "trail_color")) h->trail_c = opt_col(o, "trail_color", 0);
    if ((v = opt_find(o, "border"))) { if (v->type == VAL_STRING) snprintf(h->box, sizeof h->box, "%s", v->as.s->data); else snprintf(h->box, sizeof h->box, "%s", kc_truthy(*v) ? "single" : ""); }
    if ((v = opt_find(o, "align"))) h->align = !strcmp(v->as.s->data, "center") ? 1 : !strcmp(v->as.s->data, "right") ? 2 : 0;
    if (opt_has(o, "bold")) { if (opt_bool(o, "bold", false)) h->attr |= AT_BOLD; else h->attr &= (uint16_t)~AT_BOLD; }
    if (opt_has(o, "value")) { h->value = opt_num(o, "value", 0); if (h->shown > h->value || !h->trail) h->shown = h->shown > h->value && h->trail ? h->shown : h->value; }
    if (opt_has(o, "max")) h->max = opt_num(o, "max", 100); if (opt_has(o, "trail")) h->trail = opt_bool(o, "trail", false);
    if (opt_has(o, "vertical")) h->vertical = opt_bool(o, "vertical", false); if (opt_has(o, "hover")) h->hover = opt_bool(o, "hover", false);
    if (opt_has(o, "sel")) h->sel = opt_int(o, "sel", 0); if (opt_has(o, "canvas")) h->canvas = opt_int(o, "canvas", 0);
    if (opt_has(o, "min")) h->smin = opt_num(o, "min", 0); if (opt_has(o, "max_y")) h->smax = opt_num(o, "max_y", 1);
    if ((v = opt_find(o, "items")) && v->type == VAL_ARRAY) { free_items(h); h->nitems = v->as.arr->count; h->items = kc_malloc((size_t)(h->nitems ? h->nitems : 1) * sizeof(char *)); for (int i = 0; i < h->nitems; i++) h->items[i] = val_text(v->as.arr->items[i]); }
    if ((v = opt_find(o, "series")) && v->type == VAL_ARRAY) { free(h->series); h->ns = v->as.arr->count; h->series = kc_malloc((size_t)(h->ns ? h->ns : 1) * sizeof(float)); for (int i = 0; i < h->ns; i++) h->series[i] = (float)V_num(v->as.arr->items[i], "series"); }
    if ((v = opt_find(o, "on_click"))) { if (!as_is_fn(*v)) as_err("hud: on_click must be a function"); h->on_click = *v; h->has_click = true; }
    if ((v = opt_find(o, "on_select"))) { if (!as_is_fn(*v)) as_err("hud: on_select must be a function"); h->on_select = *v; h->has_select = true; }
    if (h->nitems && h->sel >= h->nitems) h->sel = h->nitems - 1; if (h->sel < 0) h->sel = 0;
}
int hud_create(const char *kind, const Opts *o) {
    int k = -1; for (int i = 0; KIND_NAMES[i]; i++) if (!strcmp(KIND_NAMES[i], kind)) k = i;
    if (k < 0) as_err("hud: unknown kind \"%s\" (text, panel, bar, list, button, input, hotbar, graph, minimap, spinner, crosshair, fps)", kind);
    int id = -1; for (int i = 1; i < nhu; i++) if (!HU[i].used) { id = i; break; }
    if (id < 0) { if (nhu == 0) nhu = 1; if (nhu >= caphu) { caphu = caphu ? caphu * 2 : 16; HU = kc_realloc(HU, (size_t)caphu * sizeof(Hud)); } id = nhu++; }
    Hud *h = &HU[id]; memset(h, 0, sizeof *h); h->used = true; h->visible = true; h->kind = k; h->max = 100; h->smax = 0; h->smin = 0; h->t0 = T.time;
    if (k == H_PANEL) strcpy(h->box, "single"); if (k == H_BAR) { h->fill = col_rgb(80, 200, 100); h->empty = col_rgb(60, 60, 70); h->trail_c = col_rgb(230, 200, 90); h->w = 20; h->value = h->shown = 100; }
    if (k == H_CROSSHAIR) h->anchor = 4; if (k == H_GRAPH) { h->w = 30; h->h = 6; h->fg = col_rgb(110, 200, 255); }
    apply_opts(h, o); if (k == H_BAR && !opt_has(o, "value")) h->shown = h->value = h->max;
    if (k == H_LIST || k == H_BUTTON || k == H_INPUT) if (!focus_id) focus_id = id;
    return id;
}
void hud_set_prop(int id, const char *prop, KcValue v) {
    Hud *h = hu_get(id); if (!h) as_err("hud_set: invalid hud element %d", id);
    KcValue pair[2] = { kc_string(prop), v }; Opts o = opts_make(pair, 2, 0); apply_opts(h, &o);
}
KcValue hud_get_prop(int id, const char *p) {
    Hud *h = hu_get(id); if (!h) as_err("hud_get: invalid hud element %d", id);
    if (!strcmp(p, "text")) return kc_string(h->text ? h->text : ""); if (!strcmp(p, "value")) return kc_float(h->value); if (!strcmp(p, "max")) return kc_float(h->max);
    if (!strcmp(p, "x")) return kc_int(h->x); if (!strcmp(p, "y")) return kc_int(h->y); if (!strcmp(p, "w")) return kc_int(h->w); if (!strcmp(p, "h")) return kc_int(h->h);
    if (!strcmp(p, "sel")) return kc_int(h->sel); if (!strcmp(p, "visible")) return kc_bool(h->visible); if (!strcmp(p, "z")) return kc_int(h->z); if (!strcmp(p, "count")) return kc_int(h->nitems);
    if (!strcmp(p, "selected")) return h->sel < h->nitems ? kc_string(h->items[h->sel]) : kc_null(); if (!strcmp(p, "focused")) return kc_bool(focus_id == id);
    as_err("hud_get: unknown property \"%s\"", p);
}
void hud_remove(int id) { Hud *h = hu_get(id); if (!h) return; free(h->text); free(h->title); free_items(h); free(h->series); h->used = false; if (focus_id == id) focus_id = 0; }
void hud_focus_set(int id) { if (id == 0 || hu_get(id)) focus_id = id; }
int hud_focus_get(void) { return focus_id; }
void hud_push(int id, double v) { Hud *h = hu_get(id); if (!h) as_err("hud_push: invalid hud element %d", id); int cap = h->w > 0 ? h->w * 2 : 60; if (h->ns >= cap) { memmove(h->series, h->series + 1, (size_t)(h->ns - 1) * sizeof(float)); h->ns--; } h->series = kc_realloc(h->series, (size_t)(h->ns + 1 > cap ? cap : h->ns + 1) * sizeof(float)); h->series[h->ns++] = (float)v; }
static double *tw_target(Hud *h, int prop, double *tmp) { (void)tmp; switch (prop) { case 0: return NULL; } (void)h; return NULL; }
void hud_tween(int id, const char *prop, double to, double secs, const char *ease) {
    Hud *h = hu_get(id); if (!h) as_err("hud_tween: invalid hud element %d", id); (void)tw_target;
    static const char *P[] = { "x", "y", "w", "h", "value", "max", NULL }; int p = -1; for (int i = 0; P[i]; i++) if (!strcmp(P[i], prop)) p = i;
    if (p < 0) as_err("hud_tween: can only animate x, y, w, h, value, max (got \"%s\")", prop);
    int e = !strcmp(ease, "in") ? 1 : !strcmp(ease, "out") ? 2 : !strcmp(ease, "inout") ? 3 : 0;
    double from = p == 0 ? h->x : p == 1 ? h->y : p == 2 ? h->w : p == 3 ? h->h : p == 4 ? h->value : h->max;
    int slot = -1; for (int i = 0; i < 4; i++) if (!h->tw[i].active || h->tw[i].prop == p) { slot = i; break; } if (slot < 0) slot = 0;
    h->tw[slot] = (Tween){ p, from, to, 0, secs > 0.001 ? secs : 0.001, e, true };
}
static double ease_f(int e, double t) { t = t < 0 ? 0 : t > 1 ? 1 : t; if (e == 1) return t * t; if (e == 2) return 1 - (1 - t) * (1 - t); if (e == 3) return t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t); return t; }

/* ---------------------------------------------------------------- toasts / dialog */
void hud_notify_text(const char *s, Col fg, float secs) {
    if (ntoast >= 8) { free(toasts[0].text); memmove(toasts, toasts + 1, 7 * sizeof(Toast)); ntoast = 7; }
    toasts[ntoast].text = dupstr(s); toasts[ntoast].fg = fg; toasts[ntoast].life = toasts[ntoast].total = secs > 0 ? secs : 3; toasts[ntoast].anchor = 2; ntoast++;
}
void hud_dialog_start(const char *speaker, const char *text, KcValue choices, Col fg, double speed) {
    if (DLG.active) { free(DLG.speaker); free(DLG.text); for (int i = 0; i < DLG.nchoices; i++) free(DLG.choices[i]); free(DLG.choices); }
    memset(&DLG, 0, sizeof DLG); DLG.active = true; DLG.speaker = dupstr(speaker ? speaker : ""); DLG.text = dupstr(text); DLG.result = -1; DLG.speed = (float)(speed > 0 ? speed : 45); DLG.fg = fg;
    if (choices.type == VAL_ARRAY && choices.as.arr->count) { DLG.nchoices = choices.as.arr->count; DLG.choices = kc_malloc((size_t)DLG.nchoices * sizeof(char *)); for (int i = 0; i < DLG.nchoices; i++) DLG.choices[i] = val_text(choices.as.arr->items[i]); }
}
bool hud_active_dialog(void) { return DLG.active; }
int  hud_dialog_result(void) { return DLG.result; }
bool hud_dialog_finished(void) { return !DLG.active; }
bool hud_input_focused(void) { Hud *h = hu_get(focus_id); return hud_on && h && h->kind == H_INPUT && h->visible; }

/* ---------------------------------------------------------------- layout */
static void content_size(Hud *h, int *w, int *hh) {
    int cw = 0, ch = 0; bool bd = h->box[0];
    if (h->kind == H_TEXT || h->kind == H_PANEL || h->kind == H_BUTTON || h->kind == H_SPINNER || h->kind == H_FPS) {
        const char *t = h->text ? h->text : ""; if (h->kind == H_FPS) cw = 9, ch = 1; else { int cur = 0; ch = 1; for (const char *p = t; ; p++) { if (*p == '\n' || !*p) { if (cur > cw) cw = cur; cur = 0; if (!*p) break; ch++; } else { const char *q = p; cur += cp_width(utf8_next(&q)); p = q - 1; } } }
        if (h->kind == H_BUTTON) cw += 4; if (h->kind == H_SPINNER) cw += 2; if (h->kind == H_PANEL && h->title) { int tw = str_width(h->title) + 4; if (tw > cw) cw = tw; }
    } else if (h->kind == H_BAR) { cw = h->vertical ? 1 : 20; ch = h->vertical ? 8 : 1; }
    else if (h->kind == H_LIST) { for (int i = 0; i < h->nitems; i++) { int l = str_width(h->items[i]) + 3; if (l > cw) cw = l; } ch = h->nitems; }
    else if (h->kind == H_INPUT) { cw = 20; ch = 1; } else if (h->kind == H_HOTBAR) { for (int i = 0; i < h->nitems; i++) cw += str_width(h->items[i]) + 5; ch = 1; if (bd) {} }
    else if (h->kind == H_GRAPH) { cw = 30; ch = 6; } else if (h->kind == H_MINIMAP) { Canvas *c = cv_get(h->canvas); cw = c ? c->w : 10; ch = c ? c->h : 5; } else if (h->kind == H_CROSSHAIR) { cw = 1; ch = 1; }
    if (bd && h->kind != H_BUTTON && h->kind != H_HOTBAR) { cw += 2; ch += 2; }
    *w = h->w > 0 ? h->w : cw; *hh = h->h > 0 ? h->h : ch; if (*w < 1) *w = 1; if (*hh < 1) *hh = 1;
}
static void hud_rect(Hud *h, int *x, int *y, int *w, int *hh) {
    content_size(h, w, hh); int W = S_screen->w, H = S_screen->h, ax = h->anchor % 3, ay = h->anchor / 3;
    *x = ax == 0 ? h->x : ax == 1 ? (W - *w) / 2 + h->x : W - *w - h->x; *y = ay == 0 ? h->y : ay == 1 ? (H - *hh) / 2 + h->y : H - *hh - h->y;
}
static int hit_test(int mx, int my) {
    int best = 0, bz = -(1 << 30);
    for (int i = 1; i < nhu; i++) { Hud *h = &HU[i]; if (!h->used || !h->visible) continue; int x, y, w, hh; hud_rect(h, &x, &y, &w, &hh); if (mx >= x && mx < x + w && my >= y && my < y + hh && (h->z > bz || (h->z == bz && i > best))) { best = i; bz = h->z; } }
    return best;
}
int hud_at(int x, int y) { return hit_test(x, y); }

/* ---------------------------------------------------------------- update */
static void call_cb(KcValue fn, KcValue *args, int n) { kc_call_value(AC.ip, fn, args, n, AC.pos); }
static void list_move(Hud *h, int d) { if (!h->nitems) return; h->sel = as_clampi(h->sel + d, 0, h->nitems - 1); }
static void list_fix_scroll(Hud *h, int rows) { if (rows < 1) rows = 1; if (h->sel < h->scroll) h->scroll = h->sel; if (h->sel >= h->scroll + rows) h->scroll = h->sel - rows + 1; if (h->scroll < 0) h->scroll = 0; }
static void input_edit(Hud *h) {
    if (!h->text) h->text = dupstr(""); size_t len = strlen(h->text); if (h->cursor > (int)len) h->cursor = (int)len;
    if (term_key_repeat(131) && h->cursor > 0) { int p = h->cursor - 1; while (p > 0 && ((unsigned char)h->text[p] & 0xC0) == 0x80) p--; memmove(h->text + p, h->text + h->cursor, len - (size_t)h->cursor + 1); h->cursor = p; len = strlen(h->text); }
    if (term_key_repeat(132) && h->cursor < (int)len) { int p = h->cursor + 1; while (p < (int)len && ((unsigned char)h->text[p] & 0xC0) == 0x80) p++; memmove(h->text + h->cursor, h->text + p, len - (size_t)p + 1); len = strlen(h->text); }
    if (term_key_repeat(134) && h->cursor > 0) { int p = h->cursor - 1; while (p > 0 && ((unsigned char)h->text[p] & 0xC0) == 0x80) p--; h->cursor = p; }
    if (term_key_repeat(135) && h->cursor < (int)len) { int p = h->cursor + 1; while (p < (int)len && ((unsigned char)h->text[p] & 0xC0) == 0x80) p++; h->cursor = p; }
    if (term_key_repeat(138)) h->cursor = 0; if (term_key_repeat(139)) h->cursor = (int)len;
    if (T.typed[0]) { size_t tl = 0; char add[256]; for (const char *p = T.typed; *p; p++) if ((unsigned char)*p >= 32 && *p != 127) add[tl++] = *p; add[tl] = 0;
        if (tl) { h->text = kc_realloc(h->text, len + tl + 1); memmove(h->text + h->cursor + tl, h->text + h->cursor, len - (size_t)h->cursor + 1); memcpy(h->text + h->cursor, add, tl); h->cursor += (int)tl; } }
}
void hud_update(float dt) {
    for (int i = 1; i < nhu; i++) { Hud *h = &HU[i]; if (!h->used) continue;
        for (int k = 0; k < 4; k++) { Tween *t = &h->tw[k]; if (!t->active) continue; t->t += dt; double f = ease_f(t->ease, t->t / t->dur), v = t->from + (t->to - t->from) * f;
            switch (t->prop) { case 0: h->x = (int)lround(v); break; case 1: h->y = (int)lround(v); break; case 2: h->w = (int)lround(v); break; case 3: h->h = (int)lround(v); break; case 4: h->value = v; if (!h->trail || h->shown < v) h->shown = v; break; case 5: h->max = v; break; }
            if (t->t >= t->dur) t->active = false; }
        if (h->kind == H_BAR) { if (h->shown > h->value) { h->shown -= h->max * 0.5 * dt; if (h->shown < h->value) h->shown = h->value; } else h->shown = h->value; }
    }
    for (int i = 0; i < ntoast;) { toasts[i].life -= dt; if (toasts[i].life <= 0) { free(toasts[i].text); memmove(&toasts[i], &toasts[i + 1], (size_t)(ntoast - i - 1) * sizeof(Toast)); ntoast--; } else i++; }
    if (!hud_on) return;
    if (DLG.active) {                                                                         /* dialogs take all input */
        int total = (int)strlen(DLG.text); if (!DLG.done) { DLG.shown += dt * DLG.speed; if ((int)DLG.shown >= total) { DLG.shown = (float)total; DLG.done = true; } }
        bool adv = term_key_pressed(128) || term_key_pressed(32) || term_mouse_pressed(1);
        if (DLG.done && DLG.nchoices) { if (term_key_repeat(136) || term_key_repeat('w')) DLG.sel = (DLG.sel + DLG.nchoices - 1) % DLG.nchoices; if (term_key_repeat(137) || term_key_repeat('s')) DLG.sel = (DLG.sel + 1) % DLG.nchoices;
            for (int c = 0; c < DLG.nchoices && c < 9; c++) if (term_key_pressed('1' + c)) { DLG.sel = c; adv = true; } }
        if (adv) { if (!DLG.done) { DLG.shown = (float)total; DLG.done = true; } else { DLG.result = DLG.nchoices ? DLG.sel : 0; DLG.active = false; } }
        return;
    }
    if (term_key_pressed(130)) { int n = 0, order[256]; for (int i = 1; i < nhu && n < 256; i++) if (HU[i].used && HU[i].visible && (HU[i].kind == H_LIST || HU[i].kind == H_BUTTON || HU[i].kind == H_INPUT)) order[n++] = i;
        if (n) { int cur = -1; for (int i = 0; i < n; i++) if (order[i] == focus_id) cur = i; int d = (T.mods & 1) ? -1 : 1; focus_id = order[((cur < 0 ? (d > 0 ? -1 : 0) : cur) + d + n) % n]; } }
    if (term_mouse_pressed(1) || term_mouse_pressed(3)) { int id = hit_test(T.mx, T.my); Hud *h = hu_get(id);
        if (h && (h->kind == H_LIST || h->kind == H_BUTTON || h->kind == H_INPUT || h->kind == H_HOTBAR)) { focus_id = id; int x, y, w, hh; hud_rect(h, &x, &y, &w, &hh);
            if (h->kind == H_BUTTON && h->has_click) call_cb(h->on_click, NULL, 0);
            else if (h->kind == H_LIST) { int row = T.my - y - (h->box[0] ? 1 : 0) + h->scroll; if (row >= 0 && row < h->nitems) { h->sel = row; if (h->has_select) { KcValue a[1] = { kc_int(row) }; call_cb(h->on_select, a, 1); } } }
            else if (h->kind == H_HOTBAR) { int cx = x; for (int s = 0; s < h->nitems; s++) { int sw = str_width(h->items[s]) + 5; if (T.mx >= cx && T.mx < cx + sw) { h->sel = s; if (h->has_select) { KcValue a[1] = { kc_int(s) }; call_cb(h->on_select, a, 1); } } cx += sw; } }
            else if (h->kind == H_INPUT) { int off = T.mx - x - (h->box[0] ? 1 : 0); const char *t = h->text ? h->text : ""; int cells = 0; const char *p = t; while (*p && cells < off) { const char *q = p; cells += cp_width(utf8_next(&q)); p = q; } h->cursor = (int)(p - t); }
            if (h->has_click && h->kind == H_LIST) { KcValue a[1] = { kc_int(h->sel) }; call_cb(h->on_click, a, 1); } } }
    if (T.wheel_y) { Hud *h = hu_get(hit_test(T.mx, T.my)); if (h && h->kind == H_LIST) { h->scroll = as_clampi(h->scroll - T.wheel_y, 0, h->nitems > 0 ? h->nitems - 1 : 0); } }
    for (int i = 1; i < nhu; i++) { Hud *h = &HU[i]; if (h->used && h->visible && h->hover && h->kind == H_LIST && (T.dmx || T.dmy)) { int x, y, w, hh; hud_rect(h, &x, &y, &w, &hh); if (T.mx >= x && T.mx < x + w) { int row = T.my - y - (h->box[0] ? 1 : 0) + h->scroll; if (row >= 0 && row < h->nitems) h->sel = row; } } }
    Hud *f = hu_get(focus_id);
    if (f && f->visible) {
        if (f->kind == H_LIST) { if (term_key_repeat(136)) list_move(f, -1); if (term_key_repeat(137)) list_move(f, 1); if (term_key_repeat(140)) list_move(f, -5); if (term_key_repeat(141)) list_move(f, 5); if (term_key_pressed(138)) f->sel = 0; if (term_key_pressed(139)) f->sel = f->nitems ? f->nitems - 1 : 0;
            if ((term_key_pressed(128) || term_key_pressed(32)) && f->nitems && f->has_select) { KcValue a[1] = { kc_int(f->sel) }; call_cb(f->on_select, a, 1); } int x, y, w, hh; hud_rect(f, &x, &y, &w, &hh); list_fix_scroll(f, hh - (f->box[0] ? 2 : 0)); }
        else if (f->kind == H_BUTTON) { if ((term_key_pressed(128) || term_key_pressed(32)) && f->has_click) call_cb(f->on_click, NULL, 0); }
        else if (f->kind == H_INPUT) { input_edit(f); if (term_key_pressed(128) && f->has_select) { KcValue a[1] = { kc_string(f->text ? f->text : "") }; call_cb(f->on_select, a, 1); } }
    }
}
bool hud_handle_mouse_click(int x, int y, int b) { (void)x; (void)y; (void)b; return false; }
bool hud_handle_key(int c, const char *t, int m) { (void)c; (void)t; (void)m; return false; }
void hud_pre_poll(void) {}

/* ---------------------------------------------------------------- draw */
static void draw_text_block(Canvas *c, int x, int y, int w, int hh, const char *t, Col fg, uint16_t attr, int align) {
    const char *p = t; int row = 0;
    while (row < hh) { const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p); char tmp[512]; if (l > sizeof tmp - 1) l = sizeof tmp - 1; memcpy(tmp, p, l); tmp[l] = 0; int tw = str_width(tmp), off = align == 1 ? (w - tw) / 2 : align == 2 ? w - tw : 0; if (off < 0) off = 0; cv_text(c, x + off, y + row, tmp, l, fg, 0, attr); row++; if (!e) break; p = e + 1; }
}
static int cmp_z(const void *a, const void *b) { const Hud *x = &HU[*(const int *)a], *y = &HU[*(const int *)b]; if (x->z != y->z) return x->z - y->z; return *(const int *)a - *(const int *)b; }
static void draw_one(Hud *h, int id, Canvas *c) {
    int x, y, w, hh; hud_rect(h, &x, &y, &w, &hh); bool bd = h->box[0] && strcmp(h->box, "none"); bool foc = focus_id == id; int ix = x, iy = y, iw = w, ih = hh;
    Col bfg = foc && (h->kind == H_LIST || h->kind == H_INPUT) ? col_rgb(255, 220, 90) : h->bfg ? h->bfg : h->fg;
    if (h->kind == H_PANEL || h->kind == H_TEXT || h->kind == H_LIST || h->kind == H_INPUT || h->kind == H_GRAPH || h->kind == H_MINIMAP) {
        if (h->bg || (h->kind == H_PANEL)) cv_fill(c, x, y, w, hh, ' ', h->fg, h->bg, 0);
        if (bd) { draw_box(c, x, y, w, hh, h->box, bfg, h->bg, 0, false, h->title, h->title ? bfg : 0); ix = x + 1; iy = y + 1; iw = w - 2; ih = hh - 2; }
        else if (h->bg) {}
    }
    switch (h->kind) {
        case H_TEXT: case H_PANEL: if (h->text) draw_text_block(c, ix, iy, iw, ih, h->text, h->fg, h->attr, h->align); break;
        case H_FPS: { char b[24]; snprintf(b, sizeof b, "FPS %5.1f", T.fps); cv_text(c, x, y, b, strlen(b), h->fg ? h->fg : col_rgb(160, 220, 160), h->bg, h->attr); break; }
        case H_CROSSHAIR: cv_put(c, x, y, h->text && *h->text ? (uint32_t)(unsigned char)h->text[0] : '+', h->fg ? h->fg : col_rgb(255, 255, 255), 0, h->attr); break;
        case H_SPINNER: { static const uint32_t fr[10] = { 0x280B, 0x2819, 0x2839, 0x2838, 0x283C, 0x2834, 0x2826, 0x2827, 0x2807, 0x280F }; int f = (int)((T.time - h->t0) * 12) % 10; cv_put(c, x, y, fr[f], h->fg ? h->fg : col_rgb(120, 200, 255), h->bg, h->attr); if (h->text) cv_text(c, x + 2, y, h->text, strlen(h->text), h->fg, h->bg, h->attr); break; }
        case H_BUTTON: { Col fg = h->fg, bg = h->bg; uint16_t at = h->attr; if (foc) { at |= AT_REVERSE; } char b[300]; snprintf(b, sizeof b, "[ %s ]", h->text ? h->text : ""); cv_text(c, x, y, b, strlen(b), fg, bg, at); break; }
        case H_BAR: {
            double frac = h->max > 0 ? h->value / h->max : 0, sfrac = h->max > 0 ? h->shown / h->max : 0; frac = frac < 0 ? 0 : frac > 1 ? 1 : frac; sfrac = sfrac < frac ? frac : sfrac > 1 ? 1 : sfrac;
            if (!h->vertical) { int n = (int)floor(frac * w * 8 + 0.5), ns = (int)floor(sfrac * w * 8 + 0.5); static const uint32_t part[9] = { ' ', 0x258F, 0x258E, 0x258D, 0x258C, 0x258B, 0x258A, 0x2589, 0x2588 };
                for (int i = 0; i < w; i++) { int fillv = as_clampi(n - i * 8, 0, 8), tr = as_clampi(ns - i * 8, 0, 8); if (fillv == 8) cv_put(c, x + i, y, 0x2588, h->fill, h->empty ? h->empty : 0, h->attr); else if (fillv > 0) cv_put(c, x + i, y, part[fillv], h->fill, tr > fillv ? h->trail_c : (h->empty ? h->empty : 0), h->attr); else if (tr > 0 && h->trail) cv_put(c, x + i, y, tr >= 8 ? 0x2588 : part[tr], h->trail_c, h->empty, h->attr); else cv_put(c, x + i, y, ' ', 0, h->empty, 0); }
                if (h->text) { int tw = str_width(h->text), tx = x + (w - tw) / 2; for (const char *p = h->text; *p; ) { uint32_t cp = utf8_next(&p); Cell *q = cv_at(c, tx, y); Col bgc = q ? q->bg : 0; cv_put(c, tx, y, cp, h->fg ? h->fg : col_rgb(255, 255, 255), bgc, AT_BOLD); tx += cp_width(cp); } } }
            else { int n = (int)floor(frac * hh + 0.5), ns = (int)floor(sfrac * hh + 0.5); for (int j = 0; j < hh; j++) { int fromb = hh - 1 - j; if (fromb < n) cv_put(c, x, y + j, 0x2588, h->fill, 0, 0); else if (h->trail && fromb < ns) cv_put(c, x, y + j, 0x2588, h->trail_c, 0, 0); else cv_put(c, x, y + j, 0x2591, h->empty, 0, 0); } }
            break; }
        case H_LIST: { int rows = ih; list_fix_scroll(h, rows); for (int r = 0; r < rows; r++) { int i = h->scroll + r; if (i >= h->nitems) break; bool s = i == h->sel; char b[300]; snprintf(b, sizeof b, "%s %s", s ? "\xE2\x96\xB6" : " ", h->items[i]); if (s) cv_fill(c, ix, iy + r, iw, 1, ' ', h->fg, foc ? col_rgb(60, 70, 110) : col_rgb(40, 44, 56), 0); cv_text(c, ix, iy + r, b, strlen(b), s ? col_rgb(255, 255, 255) : h->fg, s ? (foc ? col_rgb(60, 70, 110) : col_rgb(40, 44, 56)) : 0, s ? AT_BOLD : h->attr); }
            if (h->nitems > rows && rows > 0) { int th = (int)((double)rows * rows / h->nitems); if (th < 1) th = 1; int tp = (int)((double)h->scroll * (rows - th) / (h->nitems - rows)); for (int r = 0; r < rows; r++) cv_put(c, ix + iw - 1, iy + r, r >= tp && r < tp + th ? 0x2503 : 0x2502, col_rgb(110, 110, 120), 0, 0); } break; }
        case H_INPUT: { const char *t = h->text ? h->text : ""; cv_fill(c, ix, iy, iw, ih, ' ', h->fg, h->bg ? h->bg : col_rgb(30, 32, 40), 0); int tw = str_width(t); const char *p = t; int skip = 0; int curcells = 0; { const char *q = t; while (q < t + h->cursor) curcells += cp_width(utf8_next(&q)); }
            if (curcells >= iw - 1) skip = curcells - (iw - 2); int col = 0; while (*p) { uint32_t cp = utf8_next(&p); int cw = cp_width(cp); if (col >= skip && col - skip < iw - 1) cv_put(c, ix + col - skip, iy, cp, h->fg, h->bg ? h->bg : col_rgb(30, 32, 40), 0); col += cw; } (void)tw;
            if (!*t && h->title == NULL) {} if (foc) { T.cur_show = true; T.cur_x = ix + curcells - skip; T.cur_y = iy; } break; }
        case H_HOTBAR: { int cx = x; for (int s = 0; s < h->nitems; s++) { char b[200]; snprintf(b, sizeof b, "[%d:%s]", s + 1, h->items[s]); int sw = (int)strlen(b) - (int)strlen(h->items[s]) + str_width(h->items[s]); bool sl = s == h->sel; cv_text(c, cx, y, b, strlen(b), sl ? col_rgb(255, 230, 120) : h->fg, sl ? col_rgb(70, 62, 30) : h->bg, sl ? AT_BOLD : h->attr); cx += sw + 1; } break; }
        case H_GRAPH: { if (h->ns < 2 || iw < 1 || ih < 1) break; int pw = iw * 2, ph = ih * 4; double lo = h->smin, hi = h->smax; if (hi <= lo) { lo = 1e30; hi = -1e30; for (int i = 0; i < h->ns; i++) { if (h->series[i] < lo) lo = h->series[i]; if (h->series[i] > hi) hi = h->series[i]; } if (hi - lo < 1e-9) hi = lo + 1; }
            Col *px = kc_malloc((size_t)pw * ph * sizeof(Col)); memset(px, 0, (size_t)pw * ph * sizeof(Col)); int prevx = -1, prevy = -1;
            for (int i = 0; i < pw; i++) { double fi = (double)i / (pw - 1) * (h->ns - 1); int i0 = (int)fi; int i1 = i0 + 1 < h->ns ? i0 + 1 : i0; double v = h->series[i0] + (h->series[i1] - h->series[i0]) * (fi - i0); int yy = (int)lround((1 - (v - lo) / (hi - lo)) * (ph - 1)); yy = as_clampi(yy, 0, ph - 1);
                int a = prevy < 0 ? yy : prevy, b2 = yy; if (a > b2) { int t = a; a = b2; b2 = t; } for (int k = a; k <= b2; k++) px[(size_t)k * pw + i] = h->fg | COL_SET; prevx = i; prevy = yy; } (void)prevx;
            hr_to_cells(px, pw, ph, HR_BRAILLE, c, ix, iy, 0, 0); free(px); break; }
        case H_MINIMAP: { Canvas *src = cv_get(h->canvas); if (src) cv_blit(c, src, 0, 0, iw, ih, ix, iy, false, false); break; }
    }
    (void)id;
}
static void draw_dialog(Canvas *c) {
    int W = c->w, H = c->h, bw = W - 4 > 70 ? 70 : W - 4, bx = (W - bw) / 2; if (bw < 12) return;
    char **lines; int total = (int)strlen(DLG.text), shown = (int)DLG.shown; char *part = kc_malloc((size_t)total + 1); memcpy(part, DLG.text, (size_t)shown); part[shown] = 0;
    int nl = cv_wrap_lines(part, bw - 4, &lines); int bh = nl + 2 + (DLG.nchoices && DLG.done ? DLG.nchoices : 0); if (DLG.speaker[0]) bh++; if (bh < 5) bh = 5; if (bh > H - 2) bh = H - 2;
    int by = H - bh - 1; draw_box(c, bx, by, bw, bh, "round", col_rgb(200, 200, 230), col_rgb(18, 20, 30), 0, true, DLG.speaker[0] ? DLG.speaker : NULL, col_rgb(255, 220, 120));
    for (int i = 0; i < nl && i < bh - 2; i++) cv_text(c, bx + 2, by + 1 + i, lines[i], strlen(lines[i]), DLG.fg ? DLG.fg : col_rgb(235, 235, 245), col_rgb(18, 20, 30), 0);
    if (DLG.done) { int cy = by + 1 + nl; for (int i = 0; i < DLG.nchoices && cy < by + bh - 1; i++, cy++) { char b[300]; snprintf(b, sizeof b, "%s %d. %s", i == DLG.sel ? "\xE2\x96\xB6" : " ", i + 1, DLG.choices[i]); cv_text(c, bx + 2, cy, b, strlen(b), i == DLG.sel ? col_rgb(255, 230, 120) : col_rgb(180, 180, 200), col_rgb(18, 20, 30), i == DLG.sel ? AT_BOLD : 0); }
        if (!DLG.nchoices && ((int)(T.time * 2) & 1)) cv_put(c, bx + bw - 3, by + bh - 2, 0x25BC, col_rgb(200, 200, 230), col_rgb(18, 20, 30), 0); }
    for (int i = 0; i < nl; i++) free(lines[i]); free(lines); free(part);
}
extern int game_entity_count(void);
void hud_draw(Canvas *c) {
    if (hud_on && nhu > 1) { int order[1024], n = 0; for (int i = 1; i < nhu && n < 1024; i++) if (HU[i].used && HU[i].visible) order[n++] = i; qsort(order, (size_t)n, sizeof(int), cmp_z); for (int i = 0; i < n; i++) draw_one(&HU[order[i]], order[i], c); }
    int ty = 1; for (int i = 0; i < ntoast; i++) { Toast *t = &toasts[i]; int tw = str_width(t->text) + 4, hx = c->w - tw - 1; uint16_t at = t->life < 0.5f ? AT_DIM : 0; draw_box(c, hx, ty, tw, 3, "round", t->fg ? t->fg : col_rgb(150, 200, 255), col_rgb(20, 24, 34), at, true, NULL, 0); cv_text(c, hx + 2, ty + 1, t->text, strlen(t->text), col_rgb(240, 240, 250), col_rgb(20, 24, 34), at); ty += 3; }
    if (DLG.active) draw_dialog(c);
    if (dbg) { char b[6][96]; snprintf(b[0], 96, " FPS %.1f  dt %.1fms ", T.fps, T.dt * 1000); snprintf(b[1], 96, " frame %lld  t %.1fs ", (long long)T.frame, T.time); snprintf(b[2], 96, " screen %dx%d  %s/%s ", T.w, T.h, T.backend, T.keymode); snprintf(b[3], 96, " mouse %d,%d  mods %d ", T.mx, T.my, T.mods); snprintf(b[4], 96, " out %llu B  cells %d ", (unsigned long long)T.bytes_out, T.cells_changed); snprintf(b[5], 96, " entities %d ", game_entity_count());
        int w = 0; for (int i = 0; i < 6; i++) { int l = str_width(b[i]); if (l > w) w = l; } for (int i = 0; i < 6; i++) cv_text(c, 0, i, b[i], strlen(b[i]), col_rgb(150, 255, 150), col_rgb(0, 0, 0), AT_BOLD), cv_fill(c, str_width(b[i]), i, w - str_width(b[i]), 1, ' ', 0, col_rgb(0, 0, 0), 0); }
}
