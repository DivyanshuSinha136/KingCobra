/* std.game — retained-mode HUD.
 *
 * Elements are created once (game.hud("bar", ...)) and drawn every frame automatically on top of
 * the 2D/3D scene: text, bars with damage trail, icons, panels, buttons, crosshairs, hotbars,
 * minimap (reads the streamed world), compass, world-anchored labels, toasts, floating damage
 * numbers, a typewriter dialog box with choices, and an F3-style debug overlay. Elements can be
 * anchored to 9 screen points, tweened (position / alpha / size / value), and clicked.
 */
#include "game_internal.h"

enum { H_TEXT, H_BAR, H_ICON, H_PANEL, H_BUTTON, H_CROSS, H_HOTBAR, H_MINIMAP, H_COMPASS, H_LABEL3D, H_KINDS };
static const char *KNAMES[H_KINDS] = { "text", "bar", "icon", "panel", "button", "crosshair", "hotbar", "minimap", "compass", "label3d" };

typedef struct {
    bool alive, visible, hover, pressed, shadow, vertical, rotate, trail_on, circle, show_val, has_cb;
    int kind, anchor, z, scale, tex, slots, sel, style, segs, radius;
    float x, y, w, h, val, max, shown, trail, alpha, zoom, gap, maxdist;
    char *text, *title, *label; uint32_t col, bg, border, hot;
    int *itex, *icount; V3 p3; KcValue cb;
} HEl;
typedef struct { int id, key; float from, to, t, dur; int ease; } Tween;
typedef struct { char *text; uint32_t col; float t, dur; } Toast;
typedef struct { V3 p; char *text; uint32_t col; float t, dur; } Floater;

static HEl *H; static int nh, ch; static int ui_scale = 1; static bool dbg;
static Tween TW[64]; static int ntw;
static Toast TO[8]; static int nto;
static Floater FL[32]; static int nfl;
static struct { bool active; char *speaker, *text; char **choices; int nch; float shown; int result; bool done; } D = { .result = -3 };

static HEl *el(int id) { return (id >= 0 && id < nh && H[id].alive) ? &H[id] : NULL; }
static uint32_t acol(uint32_t c, float a) { return a >= 0.999f ? c : (c & 0x00FFFFFFu) | ((uint32_t)(CA(c) * fclampf(a, 0, 1)) << 24); }

static int parse_anchor(KcValue v, const char *who) {
    if (V_isnum(v)) return iclampi((int)V_num(v, who), 0, 8);
    if (v.type != VAL_STRING) gm_err("%s: anchor must be a name like \"top-left\"", who);
    static const char *N[9][2] = { {"top-left","tl"}, {"top","t"}, {"top-right","tr"}, {"left","l"}, {"center","c"}, {"right","r"}, {"bottom-left","bl"}, {"bottom","b"}, {"bottom-right","br"} };
    for (int i = 0; i < 9; i++) if (!strcmp(v.as.s->data, N[i][0]) || !strcmp(v.as.s->data, N[i][1]) || (i == 4 && !strcmp(v.as.s->data, "middle"))) return i;
    gm_err("%s: unknown anchor \"%s\" (top-left top top-right left center right bottom-left bottom bottom-right)", who, v.as.s->data);
}
static void origin(const HEl *e, float w, float h, int *ox, int *oy) {
    int S = ui_scale, ax = (e->anchor % 3) == 0 ? 0 : (e->anchor % 3) == 1 ? G.w / 2 : G.w, ay = (e->anchor / 3) == 0 ? 0 : (e->anchor / 3) == 1 ? G.h / 2 : G.h;
    int hx = e->anchor % 3, hy = e->anchor / 3;
    *ox = ax + (hx == 0 ? (int)(e->x * S) : hx == 1 ? (int)(e->x * S - w / 2) : (int)(-e->x * S - w));
    *oy = ay + (hy == 0 ? (int)(e->y * S) : hy == 1 ? (int)(e->y * S - h / 2) : (int)(-e->y * S - h));
}

static HEl *hnew(int kind, int anchor, float x, float y, float w, float h) {
    int id = -1; for (int i = 0; i < nh; i++) if (!H[i].alive) { id = i; break; }
    if (id < 0) { if (nh == ch) { ch = ch ? ch * 2 : 32; H = kc_realloc(H, sizeof(HEl) * (size_t)ch); } id = nh++; }
    HEl *e = &H[id]; memset(e, 0, sizeof *e); e->alive = true; e->visible = true; e->kind = kind; e->anchor = anchor; e->x = x; e->y = y; e->w = w; e->h = h;
    e->alpha = 1; e->scale = 1; e->col = 0xFFFFFFFFu; e->bg = 0xB0101018u; e->border = 0xFFFFFFFFu; e->hot = 0xFF4C8DFFu; e->shadow = true; e->tex = -1; e->zoom = 1; e->maxdist = 60; e->trail_on = true; e->cb = kc_null();
    e->text = kc_strdup(""); return id < 0 ? NULL : e;
}
static int hid(HEl *e) { return (int)(e - H); }

int hud_add(const char *kind, KcValue *a, int n) {
    int k = -1; for (int i = 0; i < H_KINDS; i++) if (!strcmp(kind, KNAMES[i])) k = i;
    if (k < 0) gm_err("hud: unknown element \"%s\" (text bar icon panel button crosshair hotbar minimap compass label3d)", kind);
    HEl *e; Opts o;
    switch (k) {
        case H_TEXT: { A_argc(n, 4, 40, "hud(\"text\", anchor, x, y, text, [options])"); e = hnew(k, parse_anchor(a[0], "hud"), (float)A_num(a, n, 1, "hud"), (float)A_num(a, n, 2, "hud"), 0, 0);
            free(e->text); char *s = kc_to_display_string(a[3]); e->text = s; o = opts_make(a, n, 4);
            e->col = opt_col(&o, "color", 0xFFFFFFFFu); e->scale = imaxi(1, opt_int(&o, "scale", 1)); e->shadow = opt_bool(&o, "shadow", true); e->bg = opt_col(&o, "bg", 0); break; }
        case H_BAR: { A_argc(n, 7, 40, "hud(\"bar\", anchor, x, y, w, h, value, max, [options])"); e = hnew(k, parse_anchor(a[0], "hud"), (float)A_num(a, n, 1, "hud"), (float)A_num(a, n, 2, "hud"), (float)A_num(a, n, 3, "hud"), (float)A_num(a, n, 4, "hud"));
            e->val = (float)A_num(a, n, 5, "hud"); e->max = (float)A_num(a, n, 6, "hud"); if (e->max <= 0) e->max = 1; e->shown = e->trail = e->val; o = opts_make(a, n, 7);
            e->col = opt_col(&o, "color", 0xFF43C463u); e->bg = opt_col(&o, "bg", 0xC0101018u); e->border = opt_col(&o, "border", 0xFFE8E8F0u); e->trail_on = opt_bool(&o, "trail", true);
            e->show_val = opt_bool(&o, "numbers", false); e->vertical = opt_bool(&o, "vertical", false); e->segs = opt_int(&o, "segments", 0); free(e->text); e->text = kc_strdup(opt_str(&o, "label", "")); break; }
        case H_ICON: { A_argc(n, 4, 40, "hud(\"icon\", anchor, x, y, texture, [options])"); e = hnew(k, parse_anchor(a[0], "hud"), (float)A_num(a, n, 1, "hud"), (float)A_num(a, n, 2, "hud"), 0, 0);
            e->tex = A_int(a, n, 3, "hud"); Tex *t = tex_get(e->tex); if (!t) gm_err("hud icon: invalid texture %d", e->tex); o = opts_make(a, n, 4);
            e->w = (float)opt_num(&o, "w", t->w); e->h = (float)opt_num(&o, "h", t->h); e->col = opt_col(&o, "tint", 0xFFFFFFFFu); break; }
        case H_PANEL: { A_argc(n, 5, 40, "hud(\"panel\", anchor, x, y, w, h, [options])"); e = hnew(k, parse_anchor(a[0], "hud"), (float)A_num(a, n, 1, "hud"), (float)A_num(a, n, 2, "hud"), (float)A_num(a, n, 3, "hud"), (float)A_num(a, n, 4, "hud"));
            o = opts_make(a, n, 5); e->bg = opt_col(&o, "color", 0xB0101018u); e->border = opt_col(&o, "border", 0xFF5A5F78u); e->radius = opt_int(&o, "radius", 3); e->title = kc_strdup(opt_str(&o, "title", "")); break; }
        case H_BUTTON: { A_argc(n, 6, 7, "hud(\"button\", anchor, x, y, w, h, text, [callback])"); e = hnew(k, parse_anchor(a[0], "hud"), (float)A_num(a, n, 1, "hud"), (float)A_num(a, n, 2, "hud"), (float)A_num(a, n, 3, "hud"), (float)A_num(a, n, 4, "hud"));
            free(e->text); e->text = kc_to_display_string(a[5]); e->bg = 0xD0262A3Cu; e->border = 0xFF6A70A0u; e->radius = 3; if (n > 6) { e->cb = a[6]; e->has_cb = true; } break; }
        case H_CROSS: { A_argc(n, 0, 40, "hud(\"crosshair\", [style], [options])"); int st = 0; int os = 0;
            if (n > 0 && a[0].type == VAL_STRING && (!strcmp(a[0].as.s->data, "cross") || !strcmp(a[0].as.s->data, "dot") || !strcmp(a[0].as.s->data, "circle") || !strcmp(a[0].as.s->data, "plus"))) { st = !strcmp(a[0].as.s->data, "cross") ? 0 : !strcmp(a[0].as.s->data, "dot") ? 1 : !strcmp(a[0].as.s->data, "circle") ? 2 : 3; os = 1; }
            e = hnew(k, 4, 0, 0, 0, 0); e->style = st; o = opts_make(a, n, os); e->w = e->h = (float)opt_num(&o, "size", 8); e->gap = (float)opt_num(&o, "gap", 3); e->col = opt_col(&o, "color", 0xE0FFFFFFu); break; }
        case H_HOTBAR: { A_argc(n, 5, 40, "hud(\"hotbar\", anchor, x, y, slots, size, [options])"); int sl = iclampi(A_int(a, n, 3, "hud"), 1, 16), sz = A_int(a, n, 4, "hud");
            e = hnew(k, parse_anchor(a[0], "hud"), (float)A_num(a, n, 1, "hud"), (float)A_num(a, n, 2, "hud"), (float)(sl * (sz + 4) + 4), (float)(sz + 8)); e->slots = sl; e->gap = (float)sz;
            e->itex = kc_malloc(sizeof(int) * (size_t)sl); e->icount = kc_malloc(sizeof(int) * (size_t)sl); for (int i = 0; i < sl; i++) { e->itex[i] = -1; e->icount[i] = 0; } e->has_cb = false; break; }
        case H_MINIMAP: { A_argc(n, 4, 40, "hud(\"minimap\", anchor, x, y, size, [options])"); e = hnew(k, parse_anchor(a[0], "hud"), (float)A_num(a, n, 1, "hud"), (float)A_num(a, n, 2, "hud"), (float)A_num(a, n, 3, "hud"), (float)A_num(a, n, 3, "hud"));
            o = opts_make(a, n, 4); e->zoom = (float)opt_num(&o, "scale", 1.0); e->rotate = opt_bool(&o, "rotate", true); e->circle = !strcmp(opt_str(&o, "shape", "square"), "circle"); e->border = opt_col(&o, "border", 0xFFE8E8F0u); break; }
        case H_COMPASS: { A_argc(n, 4, 40, "hud(\"compass\", anchor, x, y, width, [options])"); e = hnew(k, parse_anchor(a[0], "hud"), (float)A_num(a, n, 1, "hud"), (float)A_num(a, n, 2, "hud"), (float)A_num(a, n, 3, "hud"), 18); break; }
        default: { A_argc(n, 4, 40, "hud(\"label3d\", x, y, z, text, [options])"); e = hnew(k, 4, 0, 0, 0, 0); e->p3 = v3((float)A_num(a, n, 0, "hud"), (float)A_num(a, n, 1, "hud"), (float)A_num(a, n, 2, "hud")); free(e->text); e->text = kc_to_display_string(a[3]);
            o = opts_make(a, n, 4); e->col = opt_col(&o, "color", 0xFFFFFFFFu); e->maxdist = (float)opt_num(&o, "maxdist", 40); e->scale = imaxi(1, opt_int(&o, "scale", 1)); break; }
    }
    return hid(e);
}

/* ------------------------------------------------------------ properties */
static const char *PROPS = "text value max visible x y w h color bg border alpha scale selected item on_click anchor z tex label zoom shadow title size";
bool hud_set(int id, const char *k, KcValue v) {
    HEl *e = el(id); if (!e) return false;
    if (!strcmp(k, "text")) { free(e->text); e->text = kc_to_display_string(v); }
    else if (!strcmp(k, "label")) { free(e->text); e->text = kc_to_display_string(v); }
    else if (!strcmp(k, "title")) { free(e->title); e->title = kc_to_display_string(v); }
    else if (!strcmp(k, "value")) { e->val = (float)V_num(v, k); if (e->val > e->max && e->kind == H_BAR) e->val = e->max; if (e->val < 0) e->val = 0; if (e->val >= e->shown) e->trail = e->val; }
    else if (!strcmp(k, "max")) e->max = fmaxf(1e-6f, (float)V_num(v, k));
    else if (!strcmp(k, "visible")) e->visible = kc_truthy(v);
    else if (!strcmp(k, "x")) e->x = (float)V_num(v, k); else if (!strcmp(k, "y")) e->y = (float)V_num(v, k);
    else if (!strcmp(k, "w") || !strcmp(k, "size")) e->w = (float)V_num(v, k); else if (!strcmp(k, "h")) e->h = (float)V_num(v, k);
    else if (!strcmp(k, "color")) e->col = col_from_value(v, k); else if (!strcmp(k, "bg")) e->bg = col_from_value(v, k); else if (!strcmp(k, "border")) e->border = col_from_value(v, k);
    else if (!strcmp(k, "alpha")) e->alpha = fclampf((float)V_num(v, k), 0, 1); else if (!strcmp(k, "scale")) e->scale = imaxi(1, (int)V_num(v, k)); else if (!strcmp(k, "zoom")) e->zoom = fmaxf(0.1f, (float)V_num(v, k));
    else if (!strcmp(k, "selected")) e->sel = iclampi((int)V_num(v, k), 0, imaxi(0, e->slots - 1));
    else if (!strcmp(k, "item")) {
        if (e->kind != H_HOTBAR || v.type != VAL_ARRAY || v.as.arr->count < 2) gm_err("hud set item: expected [slot, texture, count] on a hotbar");
        int s = (int)V_num(v.as.arr->items[0], k); if (s < 0 || s >= e->slots) gm_err("hud set item: slot %d out of range", s);
        e->itex[s] = (int)V_num(v.as.arr->items[1], k); e->icount[s] = v.as.arr->count > 2 ? (int)V_num(v.as.arr->items[2], k) : 1;
    }
    else if (!strcmp(k, "on_click")) { e->cb = v; e->has_cb = v.type != VAL_NULL; }
    else if (!strcmp(k, "anchor")) e->anchor = parse_anchor(v, k); else if (!strcmp(k, "z")) e->z = (int)V_num(v, k);
    else if (!strcmp(k, "tex")) e->tex = (int)V_num(v, k); else if (!strcmp(k, "shadow")) e->shadow = kc_truthy(v);
    else gm_err("hud set: unknown property \"%s\" (%s)", k, PROPS);
    return true;
}
KcValue hud_get(int id, const char *k) {
    HEl *e = el(id); if (!e) return kc_null();
    if (!strcmp(k, "text") || !strcmp(k, "label")) return kc_string(e->text); if (!strcmp(k, "value")) return kc_float(e->val); if (!strcmp(k, "max")) return kc_float(e->max);
    if (!strcmp(k, "visible")) return kc_bool(e->visible); if (!strcmp(k, "x")) return kc_float(e->x); if (!strcmp(k, "y")) return kc_float(e->y); if (!strcmp(k, "w")) return kc_float(e->w); if (!strcmp(k, "h")) return kc_float(e->h);
    if (!strcmp(k, "alpha")) return kc_float(e->alpha); if (!strcmp(k, "selected")) return kc_int(e->sel); if (!strcmp(k, "hover")) return kc_bool(e->hover);
    if (!strcmp(k, "kind")) return kc_string(KNAMES[e->kind]); if (!strcmp(k, "color")) return kc_int(e->col);
    if (!strcmp(k, "item") ) return kc_null();
    gm_err("hud get: unknown property \"%s\"", k);
}
void hud_remove(int id) { HEl *e = el(id); if (!e) return; free(e->text); free(e->title); free(e->itex); free(e->icount); e->alive = false; }
void hud_clear(void) { for (int i = 0; i < nh; i++) hud_remove(i); ntw = 0; }
void hud_scale(int s) { ui_scale = iclampi(s, 1, 6); }
void hud_debug(bool on) { dbg = on; }

/* ---------------------------------------------------------------- tweens */
static float ease(int k, float t) { t = fclampf(t, 0, 1); switch (k) { case 1: return t * t; case 2: return 1 - (1 - t) * (1 - t); case 3: return t < 0.5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t); case 4: { float c = 1.70158f; return 1 + (c + 1) * powf(t - 1, 3) + c * powf(t - 1, 2); } default: return t; } }
static const char *TK[] = { "x", "y", "w", "h", "alpha", "value" };
void hud_tween(int id, const char *key, float target, float secs, const char *e) {
    HEl *el_ = el(id); if (!el_) return; int ki = -1; for (int i = 0; i < 6; i++) if (!strcmp(key, TK[i])) ki = i;
    if (ki < 0) gm_err("hud tween: property \"%s\" can't be animated (x y w h alpha value)", key);
    int ek = !strcmp(e, "in") ? 1 : !strcmp(e, "out") ? 2 : !strcmp(e, "inout") ? 3 : !strcmp(e, "back") ? 4 : 0;
    float from = ki == 0 ? el_->x : ki == 1 ? el_->y : ki == 2 ? el_->w : ki == 3 ? el_->h : ki == 4 ? el_->alpha : el_->val;
    for (int i = 0; i < ntw; i++) if (TW[i].id == id && TW[i].key == ki) { TW[i] = TW[--ntw]; break; }
    if (secs <= 0) { KcValue v = kc_float(target); hud_set(id, key, v); return; }
    if (ntw < 64) { TW[ntw].id = id; TW[ntw].key = ki; TW[ntw].from = from; TW[ntw].to = target; TW[ntw].t = 0; TW[ntw].dur = secs; TW[ntw].ease = ek; ntw++; }
}

/* ---------------------------------------------------- notifications etc. */
void hud_notify(const char *text, uint32_t col, float secs) {
    if (nto == 8) { free(TO[0].text); memmove(TO, TO + 1, sizeof(Toast) * 7); nto--; }
    TO[nto].text = kc_strdup(text); TO[nto].col = col; TO[nto].t = 0; TO[nto].dur = secs > 0 ? secs : 3; nto++;
}
void hud_float_text(V3 p, const char *text, uint32_t col, float secs) {
    if (nfl == 32) { free(FL[0].text); memmove(FL, FL + 1, sizeof(Floater) * 31); nfl--; }
    FL[nfl].p = p; FL[nfl].text = kc_strdup(text); FL[nfl].col = col; FL[nfl].t = 0; FL[nfl].dur = secs > 0 ? secs : 1.2f; nfl++;
}
void hud_dialog_open(const char *speaker, const char *text, KcValue *ch_, int nch) {
    for (int i = 0; i < D.nch; i++) free(D.choices[i]); free(D.choices); free(D.speaker); free(D.text);
    D.speaker = kc_strdup(speaker); D.text = kc_strdup(text); D.nch = nch; D.choices = nch ? kc_malloc(sizeof(char *) * (size_t)nch) : NULL;
    for (int i = 0; i < nch; i++) D.choices[i] = kc_to_display_string(ch_[i]);
    D.active = true; D.shown = 0; D.result = -2; D.done = false;
}
bool hud_dialog_active(void) { return D.active; }
int hud_dialog_result(void) { return D.result; }
static void dialog_close(int result) { D.active = false; D.result = result; }
void hud_dialog_advance(void) {
    if (!D.active) return;
    if (D.shown < (float)strlen(D.text)) { D.shown = (float)strlen(D.text); return; }
    if (D.nch == 0) dialog_close(-1);
}
void hud_key(int idx) { if (D.active && D.shown >= (float)strlen(D.text) && idx >= 0 && idx < D.nch) dialog_close(idx); }

/* ----------------------------------------------------------------- update */
static int dialog_choice_at(int mx, int my, int *bx, int *by, int *bw, int *bh);
void hud_update(float dt) {
    for (int i = 0; i < ntw;) {
        Tween *t = &TW[i]; t->t += dt; float k = ease(t->ease, t->t / t->dur); KcValue v = kc_float(t->from + (t->to - t->from) * k); hud_set(t->id, TK[t->key], v);
        if (t->t >= t->dur) TW[i] = TW[--ntw]; else i++;
    }
    for (int i = 0; i < nh; i++) { HEl *e = &H[i]; if (!e->alive) continue;
        if (e->kind == H_BAR) { e->shown += (e->val - e->shown) * fminf(1.0f, dt * 12.0f); if (fabsf(e->shown - e->val) < 0.001f * e->max) e->shown = e->val;
            if (e->trail > e->shown) e->trail = fmaxf(e->shown, e->trail - dt * e->max * 0.35f); else e->trail = e->shown; } }
    for (int i = 0; i < nto;) { TO[i].t += dt; if (TO[i].t > TO[i].dur) { free(TO[i].text); memmove(TO + i, TO + i + 1, sizeof(Toast) * (size_t)(nto - i - 1)); nto--; } else i++; }
    for (int i = 0; i < nfl;) { FL[i].t += dt; FL[i].p.y += dt * 1.1f; if (FL[i].t > FL[i].dur) { free(FL[i].text); memmove(FL + i, FL + i + 1, sizeof(Floater) * (size_t)(nfl - i - 1)); nfl--; } else i++; }
    if (D.active) D.shown += dt * 45.0f;
    int hv = hud_hit(G.mx, G.my); for (int i = 0; i < nh; i++) if (H[i].alive) { H[i].hover = (i == hv); if (!G.mb[1]) H[i].pressed = false; }
}

/* ------------------------------------------------------------------- draw */
static void frame(int x, int y, int w, int h, uint32_t bg, uint32_t border, int r) {
    if (CA(bg)) { if (r > 0 && w > 2 * r && h > 2 * r) { gfx_rect(x + r, y, w - 2 * r, h, bg, true); gfx_rect(x, y + r, r, h - 2 * r, bg, true); gfx_rect(x + w - r, y + r, r, h - 2 * r, bg, true);
            for (int i = 1; i < r; i++) { gfx_rect(x + r - i, y + i, i, 1, bg, true); } for (int i = 0; i < r; i++) { gfx_rect(x + r - (r - i), y + i, r - i, 1, bg, true); gfx_rect(x + w - r, y + i, r - i, 1, bg, true); gfx_rect(x + r - (r - i), y + h - 1 - i, r - i, 1, bg, true); gfx_rect(x + w - r, y + h - 1 - i, r - i, 1, bg, true); } }
        else gfx_rect(x, y, w, h, bg, true); }
    if (CA(border)) gfx_rect(x, y, w, h, border, false);
}
static void draw_text_el(HEl *e, int x, int y, const char *s, uint32_t col, int sc) { if (e->shadow) gfx_text_shadow(x, y, s, col, sc, acol(0xFF000000u, e->alpha * 0.8f)); else gfx_text(x, y, s, col, sc); }

static void draw_minimap(HEl *e, int ox, int oy, int sz) {
    int cell = imaxi(1, (int)(e->zoom + 0.5f)), n = sz / cell; float cx = G.cam.x, cz = G.cam.z; float yaw = e->rotate ? G.yaw : 0; float cs = cosf(yaw), sn = sinf(yaw);
    gfx_rect(ox, oy, sz, sz, acol(0xFF0A0C14u, e->alpha), true);
    for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) {
        float lx = (i - n / 2) * (1.0f / (e->zoom >= 1 ? 1 : e->zoom)) * (e->zoom >= 1 ? 1.0f / (float)cell * (float)cell / e->zoom : 1.0f), lz = (j - n / 2) * (1.0f / (e->zoom >= 1 ? 1 : e->zoom));
        (void)lx; (void)lz;
        float dx = (float)(i - n / 2) / (e->zoom >= 1 ? e->zoom / (float)cell : e->zoom), dz = (float)(j - n / 2) / (e->zoom >= 1 ? e->zoom / (float)cell : e->zoom);
        if (e->circle) { float rr = (float)(n / 2); float ex = (float)(i - n / 2), ez = (float)(j - n / 2); if (ex * ex + ez * ez > rr * rr) continue; }
        float wx = cx + dx * cs + dz * sn, wz = cz - dx * sn + dz * cs; uint32_t c = world_gen_color((int)floorf(wx), (int)floorf(wz));
        if (!c) c = 0xFF14161E; gfx_rect(ox + i * cell, oy + j * cell, cell, cell, acol(c, e->alpha), true);
    }
    int mx = ox + sz / 2, my = oy + sz / 2; float a = e->rotate ? 0 : -G.yaw;                         /* player arrow */
    float tx = mx + sinf(a) * 6 * ui_scale, ty = my - cosf(a) * 6 * ui_scale; gfx_tri((int)tx, (int)ty, (int)(mx + sinf(a + 2.5f) * 4 * ui_scale), (int)(my - cosf(a + 2.5f) * 4 * ui_scale), (int)(mx + sinf(a - 2.5f) * 4 * ui_scale), (int)(my - cosf(a - 2.5f) * 4 * ui_scale), acol(0xFFFF5050u, e->alpha), true);
    gfx_rect(ox, oy, sz, sz, acol(e->border, e->alpha), false);
    int nx = mx, ny = oy + 2; if (e->rotate) { nx = (int)(mx + sinf(G.yaw) * (sz / 2 - 8)); ny = (int)(my - cosf(G.yaw) * (sz / 2 - 8)) - 4; nx -= 4; }
    gfx_text_shadow(e->rotate ? nx : mx - 4, e->rotate ? ny : ny, "N", acol(0xFFFFFFFFu, e->alpha), 1, 0xFF000000u);
}
static void draw_compass(HEl *e, int ox, int oy, int w, int h) {
    gfx_rect(ox, oy, w, h, acol(0x90101018u, e->alpha), true); gfx_rect(ox, oy, w, h, acol(0xFF5A5F78u, e->alpha), false);
    float hdg = fmodf(RAD2DEG(-G.yaw) + 720.0f, 360.0f);                                              /* 0 = north (-Z), increases clockwise */
    static const char *L[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" }; float ppd = (float)w / 180.0f;
    for (int d = 0; d < 360; d += 15) { float rel = (float)d - hdg; while (rel > 180) rel -= 360; while (rel < -180) rel += 360; if (fabsf(rel) > 88) continue;
        int x = ox + w / 2 + (int)(rel * ppd); bool major = d % 45 == 0; gfx_rect(x, oy + (major ? 2 : h - 6 * ui_scale), 1, major ? 5 * ui_scale : 4 * ui_scale, acol(0xFFC8CCE0u, e->alpha), true);
        if (major) gfx_text_shadow(x - (int)strlen(L[d / 45]) * 4, oy + h / 2 - 2, L[d / 45], acol(d == 0 ? 0xFFFF6060u : 0xFFFFFFFFu, e->alpha), 1, 0xFF000000u); }
    gfx_tri(ox + w / 2, oy + h, ox + w / 2 - 3, oy + h + 5, ox + w / 2 + 3, oy + h + 5, acol(0xFFFFD040u, e->alpha), true);
    char b[16]; snprintf(b, sizeof b, "%d", (int)hdg); gfx_text_shadow(ox + w / 2 - (int)strlen(b) * 4, oy + h + 7, b, acol(0xFFFFFFFFu, e->alpha), 1, 0xFF000000u);
}
static int cmp_z(const void *a, const void *b) { const HEl *x = *(HEl *const *)a, *y = *(HEl *const *)b; return x->z != y->z ? x->z - y->z : (int)(x - y); }

static void draw_el(HEl *e) {
    int S = ui_scale; int x, y; float a = e->alpha;
    switch (e->kind) {
        case H_TEXT: { int sc = e->scale * S, w = gfx_text_w(e->text, sc), h = gfx_text_h(e->text, sc); origin(e, (float)w, (float)h, &x, &y);
            if (CA(e->bg)) gfx_rect(x - 3 * S, y - 2 * S, w + 6 * S, h + 4 * S, acol(e->bg, a), true); draw_text_el(e, x, y, e->text, acol(e->col, a), sc); break; }
        case H_BAR: { int w = (int)(e->w * S), h = (int)(e->h * S); origin(e, (float)w, (float)h, &x, &y); frame(x, y, w, h, acol(e->bg, a), 0, 0);
            float f = e->shown / e->max, ft = e->trail / e->max; f = fclampf(f, 0, 1); ft = fclampf(ft, 0, 1);
            if (e->vertical) { int fh = (int)((h - 2) * f), th = (int)((h - 2) * ft); if (e->trail_on && th > fh) gfx_rect(x + 1, y + h - 1 - th, w - 2, th - fh, acol(0xFFE8E8E8u, a * 0.85f), true); gfx_rect(x + 1, y + h - 1 - fh, w - 2, fh, acol(e->col, a), true); }
            else { int fw = (int)((w - 2) * f), tw = (int)((w - 2) * ft); if (e->trail_on && tw > fw) gfx_rect(x + 1 + fw, y + 1, tw - fw, h - 2, acol(0xFFE8E8E8u, a * 0.85f), true); gfx_rect(x + 1, y + 1, fw, h - 2, acol(e->col, a), true);
                gfx_rect(x + 1, y + 1, fw, imaxi(1, (h - 2) / 3), acol(0x40FFFFFFu, a), true); }
            if (e->segs > 1) for (int i = 1; i < e->segs; i++) { if (e->vertical) gfx_rect(x + 1, y + i * h / e->segs, w - 2, 1, acol(0xA0000000u, a), true); else gfx_rect(x + i * w / e->segs, y + 1, 1, h - 2, acol(0xA0000000u, a), true); }
            gfx_rect(x, y, w, h, acol(e->border, a), false);
            char buf[96]; buf[0] = 0; if (e->show_val) snprintf(buf, sizeof buf, "%s%s%d/%d", e->text, *e->text ? " " : "", (int)(e->val + 0.5f), (int)(e->max + 0.5f)); else if (*e->text) snprintf(buf, sizeof buf, "%s", e->text);
            if (*buf) { int sc = imaxi(1, S * (h >= 22 * S ? 2 : 1)); int tw = gfx_text_w(buf, sc), th = gfx_text_h(buf, sc); if (th < h) draw_text_el(e, x + (w - tw) / 2, y + (h - th) / 2, buf, acol(0xFFFFFFFFu, a), sc); }
            break; }
        case H_ICON: { Tex *t = tex_get(e->tex); int w = (int)(e->w * S), h = (int)(e->h * S); origin(e, (float)w, (float)h, &x, &y); gfx_blit(t, 0, 0, 0, 0, (float)x, (float)y, (float)w, (float)h, 0, 0, acol(e->col, a)); break; }
        case H_PANEL: { int w = (int)(e->w * S), h = (int)(e->h * S); origin(e, (float)w, (float)h, &x, &y); frame(x, y, w, h, acol(e->bg, a), acol(e->border, a), e->radius * S);
            if (e->title && *e->title) { int tw = gfx_text_w(e->title, S); gfx_rect(x + 1, y + 1, w - 2, 10 * S, acol(0x60FFFFFFu, a * 0.25f), true); draw_text_el(e, x + (w - tw) / 2, y + 2 * S, e->title, acol(0xFFFFFFFFu, a), S); } break; }
        case H_BUTTON: { int w = (int)(e->w * S), h = (int)(e->h * S); origin(e, (float)w, (float)h, &x, &y); uint32_t bg = e->pressed ? 0xFF4C8DFFu : e->hover ? 0xE03A4066u : e->bg;
            frame(x, y, w, h, acol(bg, a), acol(e->hover ? 0xFFA0B0FFu : e->border, a), e->radius * S); int sc = S, tw = gfx_text_w(e->text, sc), th = gfx_text_h(e->text, sc); draw_text_el(e, x + (w - tw) / 2, y + (h - th) / 2, e->text, acol(e->col, a), sc); break; }
        case H_CROSS: { int s = (int)(e->w * S), g = (int)(e->gap * S); int cx = G.w / 2, cy = G.h / 2; uint32_t c = acol(e->col, a), sh = acol(0x90000000u, a);
            if (e->style == 1) { gfx_rect(cx - S, cy - S, 2 * S, 2 * S, sh, true); gfx_rect(cx - S + 1, cy - S + 1, 2 * S - 2 > 0 ? 2 * S - 2 : 1, 2 * S - 2 > 0 ? 2 * S - 2 : 1, c, true); }
            else if (e->style == 2) { gfx_circle(cx, cy, s, sh, false); gfx_circle(cx, cy, s - 1, c, false); }
            else if (e->style == 3) { gfx_rect(cx - s, cy - S / 2, 2 * s, S, c, true); gfx_rect(cx - S / 2, cy - s, S, 2 * s, c, true); }
            else { int t = imaxi(1, S); gfx_rect(cx - g - s - 1, cy - t / 2 - 1, s + 2, t + 2, sh, true); gfx_rect(cx + g - 1, cy - t / 2 - 1, s + 2, t + 2, sh, true); gfx_rect(cx - t / 2 - 1, cy - g - s - 1, t + 2, s + 2, sh, true); gfx_rect(cx - t / 2 - 1, cy + g - 1, t + 2, s + 2, sh, true);
                gfx_rect(cx - g - s, cy - t / 2, s, t, c, true); gfx_rect(cx + g, cy - t / 2, s, t, c, true); gfx_rect(cx - t / 2, cy - g - s, t, s, c, true); gfx_rect(cx - t / 2, cy + g, t, s, c, true); }
            break; }
        case H_HOTBAR: { int sz = (int)(e->gap * S), pad = 4 * S, w = e->slots * (sz + pad) + pad, h = sz + 2 * pad; origin(e, (float)w, (float)h, &x, &y); frame(x, y, w, h, acol(0xA0101018u, a), acol(0xFF5A5F78u, a), 3 * S);
            for (int i = 0; i < e->slots; i++) { int sx = x + pad + i * (sz + pad), sy = y + pad; bool sel = i == e->sel; frame(sx, sy, sz, sz, acol(sel ? 0x80405090u : 0x60202434u, a), acol(sel ? 0xFFFFE070u : 0xFF3A3F58u, a), 0); if (sel) gfx_rect(sx - 1, sy - 1, sz + 2, sz + 2, acol(0xFFFFE070u, a), false);
                if (e->itex[i] >= 0) { Tex *t = tex_get(e->itex[i]); if (t) gfx_blit(t, 0, 0, 0, 0, (float)sx + 2, (float)sy + 2, (float)sz - 4, (float)sz - 4, 0, 0, acol(0xFFFFFFFFu, a)); }
                if (e->icount[i] > 1) { char b[12]; snprintf(b, sizeof b, "%d", e->icount[i]); gfx_text_shadow(sx + sz - gfx_text_w(b, S) - 2, sy + sz - 8 * S - 1, b, acol(0xFFFFFFFFu, a), S, 0xFF000000u); }
                char k[12]; snprintf(k, sizeof k, "%d", i + 1); gfx_text(sx + 2, sy + 2, k, acol(0xB0FFFFFFu, a), 1); }
            break; }
        case H_MINIMAP: { int sz = (int)(e->w * S); origin(e, (float)sz, (float)sz, &x, &y); draw_minimap(e, x, y, sz); break; }
        case H_COMPASS: { int w = (int)(e->w * S), h = (int)(e->h * S); origin(e, (float)w, (float)h, &x, &y); draw_compass(e, x, y, w, h); break; }
        case H_LABEL3D: { float sx, sy, d; if (!r3d_project(e->p3, &sx, &sy, &d) || d > e->maxdist) break; float fade = d > e->maxdist * 0.7f ? 1.0f - (d - e->maxdist * 0.7f) / (e->maxdist * 0.3f) : 1.0f;
            int sc = e->scale * S, w = gfx_text_w(e->text, sc); gfx_text_shadow((int)sx - w / 2, (int)sy - 4 * sc, e->text, acol(e->col, a * fade), sc, acol(0xFF000000u, a * fade)); break; }
    }
}

static void draw_dialog(void) {
    int S = ui_scale; int w = G.w * 8 / 10, h = imaxi(G.h / 4, (D.nch ? 40 + D.nch * 12 : 50) * S), x = (G.w - w) / 2, y = G.h - h - 8 * S;
    gfx_rect(x, y, w, h, 0xE0101020u, true); gfx_rect(x, y, w, h, 0xFF7F86B8u, false); gfx_rect(x + 2, y + 2, w - 4, h - 4, 0xFF343A5Cu, false);
    if (*D.speaker) { int tw = gfx_text_w(D.speaker, S) + 12 * S; gfx_rect(x + 10 * S, y - 6 * S, tw, 12 * S + 2, 0xFF3A4A8Cu, true); gfx_rect(x + 10 * S, y - 6 * S, tw, 12 * S + 2, 0xFFA0B0FFu, false); gfx_text_shadow(x + 16 * S, y - 3 * S, D.speaker, 0xFFFFFFFFu, S, 0xFF000000u); }
    int len = (int)strlen(D.text), shown = (int)D.shown; if (shown > len) shown = len; char *buf = kc_malloc((size_t)len + 1); memcpy(buf, D.text, (size_t)shown); buf[shown] = 0;
    /* word wrap to the box */
    int maxc = (w - 20 * S) / (8 * S); if (maxc < 8) maxc = 8; char *wrapped = kc_malloc((size_t)shown * 2 + 2); int col = 0, o = 0; for (int i = 0; i < shown; i++) { if (buf[i] == '\n') { col = 0; wrapped[o++] = '\n'; continue; } if (col >= maxc) { int j = o; while (j > 0 && wrapped[j - 1] != ' ' && wrapped[j - 1] != '\n') j--; if (j > 0 && wrapped[j - 1] == ' ' && o - j < maxc) { wrapped[j - 1] = '\n'; col = o - j; } else { wrapped[o++] = '\n'; col = 0; } } wrapped[o++] = buf[i]; col++; } wrapped[o] = 0;
    gfx_text_shadow(x + 10 * S, y + 12 * S, wrapped, 0xFFFFFFFFu, S, 0xFF000000u); free(buf); free(wrapped);
    bool done = (float)shown >= (float)len;
    if (done && D.nch == 0 && ((int)(G.time * 2) & 1)) gfx_tri(x + w - 18 * S, y + h - 14 * S, x + w - 10 * S, y + h - 14 * S, x + w - 14 * S, y + h - 8 * S, 0xFFFFE070u, true);
    if (done) for (int i = 0; i < D.nch; i++) { int bx, by, bw, bh; dialog_choice_at(-1, -1, &bx, &by, &bw, &bh); (void)bx; (void)by; (void)bw; (void)bh;
        int cy = y + h - (D.nch - i) * 12 * S - 4 * S; bool hov = G.mx >= x + 8 * S && G.mx < x + w - 8 * S && G.my >= cy - 1 && G.my < cy + 11 * S; char b[160]; snprintf(b, sizeof b, "%d. %s", i + 1, D.choices[i]);
        if (hov) gfx_rect(x + 8 * S, cy - 1, w - 16 * S, 11 * S, 0x604C6CFFu, true); gfx_text_shadow(x + 12 * S, cy, b, hov ? 0xFFFFE070u : 0xFFD8E0FFu, S, 0xFF000000u); }
}
static int dialog_choice_at(int mx, int my, int *bx, int *by, int *bw, int *bh) {
    if (!D.active || D.shown < (float)strlen(D.text)) return -1; int S = ui_scale; int w = G.w * 8 / 10, h = imaxi(G.h / 4, (D.nch ? 40 + D.nch * 12 : 50) * S), x = (G.w - w) / 2, y = G.h - h - 8 * S;
    *bx = x; *by = y; *bw = w; *bh = h;
    for (int i = 0; i < D.nch; i++) { int cy = y + h - (D.nch - i) * 12 * S - 4 * S; if (mx >= x + 8 * S && mx < x + w - 8 * S && my >= cy - 1 && my < cy + 11 * S) return i; }
    return -1;
}

void hud_draw(void) {
    gfx_clip_reset(); int S = ui_scale;
    HEl *order[512]; int no = 0; for (int i = 0; i < nh && no < 512; i++) if (H[i].alive && H[i].visible) order[no++] = &H[i];
    qsort(order, (size_t)no, sizeof(HEl *), cmp_z);
    for (int i = 0; i < no; i++) draw_el(order[i]);
    for (int i = 0; i < nfl; i++) { float sx, sy, d; if (!r3d_project(FL[i].p, &sx, &sy, &d)) continue; float k = FL[i].t / FL[i].dur; int sc = d < 12 ? 2 * S : S; int w = gfx_text_w(FL[i].text, sc); uint32_t c = acol(FL[i].col, 1.0f - k * k); gfx_text_shadow((int)sx - w / 2, (int)sy, FL[i].text, c, sc, acol(0xFF000000u, 1.0f - k)); }
    for (int i = 0; i < nto; i++) { float k = TO[i].t < 0.25f ? TO[i].t / 0.25f : (TO[i].t > TO[i].dur - 0.5f ? (TO[i].dur - TO[i].t) / 0.5f : 1.0f); int w = gfx_text_w(TO[i].text, S) + 14 * S, h = 14 * S; int x = G.w - w - 8 * S + (int)((1.0f - fclampf(k, 0, 1)) * 30), y = 8 * S + (nto - 1 - i) * (h + 4 * S) + 20 * S;
        gfx_rect(x, y, w, h, acol(0xD0141828u, k), true); gfx_rect(x, y, 3 * S, h, acol(TO[i].col, k), true); gfx_rect(x, y, w, h, acol(0xFF4A5070u, k), false); gfx_text_shadow(x + 8 * S, y + 3 * S + 1, TO[i].text, acol(0xFFFFFFFFu, k), S, acol(0xFF000000u, k)); }
    if (D.active) draw_dialog();
    if (dbg) {
        char b[600]; int ld, nd, rd; size_t by; world_counts(&ld, &nd, &rd, &by);
        snprintf(b, sizeof b, "FPS %.0f  frame %.1f ms  (render %.1f, stream %.1f)\nPOS %.1f %.1f %.1f  yaw %.0f pitch %.0f\nCHUNK %d,%d  loaded %d  ready %d/%d  mem %.1f MB\nTRIS %d drawn %d  quads %d  draws %d  ents %d  parts %d\nFOG %s  end %.0f  chunks drawn %d culled %d\nTEX %.1f MB  MODELS %.1f MB",
            G.fps, G.st_frame_ms, G.st_render_ms, G.st_stream_ms, G.cam.x, G.cam.y, G.cam.z, RAD2DEG(G.yaw), RAD2DEG(G.pitch), floordiv((int)floorf(G.cam.x), 16), floordiv((int)floorf(G.cam.z), 16), ld, rd, nd, (double)by / 1048576.0,
            G.st_tris, G.st_tris_drawn, G.st_quads, G.st_draws, G.st_ents_drawn, G.st_particles, R_fog.mode == 0 ? "off" : R_fog.mode == 1 ? "linear" : R_fog.mode == 2 ? "exp" : "cloud", R_fog.autofog ? R_fog.cur_end : R_fog.end, G.st_chunks_drawn, G.st_chunks_culled,
            (double)tex_resident_bytes() / 1048576.0, (double)model_resident_bytes() / 1048576.0);
        int w = gfx_text_w(b, S) + 8 * S, h = gfx_text_h(b, S) + 6 * S; gfx_rect(4, 4, w, h, 0xA0000000u, true); gfx_text(8, 4 + 3 * S, b, 0xFFE8F0FFu, S);
    }
}

/* ------------------------------------------------------------------ input */
int hud_hit(int mx, int my) {
    int S = ui_scale;
    for (int i = nh - 1; i >= 0; i--) { HEl *e = &H[i]; if (!e->alive || !e->visible) continue; if (!(e->kind == H_BUTTON || e->kind == H_HOTBAR || e->has_cb)) continue;
        int w, h, x, y; if (e->kind == H_HOTBAR) { int sz = (int)(e->gap * S), pad = 4 * S; w = e->slots * (sz + pad) + pad; h = sz + 2 * pad; }
        else if (e->kind == H_TEXT) { w = gfx_text_w(e->text, e->scale * S); h = gfx_text_h(e->text, e->scale * S); } else { w = (int)(e->w * S); h = (int)(e->h * S); }
        origin(e, (float)w, (float)h, &x, &y); if (mx >= x && mx < x + w && my >= y && my < y + h) return i; }
    return -1;
}
bool hud_click(int mx, int my) {
    if (D.active) { int bx = 0, by = 0, bw = 0, bh = 0; int c = dialog_choice_at(mx, my, &bx, &by, &bw, &bh); if (c >= 0) { dialog_close(c); return true; } if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) { hud_dialog_advance(); return true; } if (bw == 0) { hud_dialog_advance(); return true; } }
    int id = hud_hit(mx, my); if (id < 0) return false; HEl *e = &H[id]; int S = ui_scale; e->pressed = true;
    int arg = -1;
    if (e->kind == H_HOTBAR) { int w, h, x, y; int sz = (int)(e->gap * S), pad = 4 * S; w = e->slots * (sz + pad) + pad; h = sz + 2 * pad; origin(e, (float)w, (float)h, &x, &y); int s = (mx - x - pad / 2) / (sz + pad); e->sel = iclampi(s, 0, e->slots - 1); arg = e->sel; }
    if (e->has_cb && G.ip) { KcValue args[2] = { kc_int(id), kc_int(arg) }; kc_call_value(G.ip, e->cb, args, 2, G.pos); }
    return true;
}
void hud_reset(void) {
    hud_clear(); free(H); H = NULL; nh = ch = 0; for (int i = 0; i < nto; i++) free(TO[i].text); nto = 0; for (int i = 0; i < nfl; i++) free(FL[i].text); nfl = 0;
    for (int i = 0; i < D.nch; i++) free(D.choices[i]); free(D.choices); free(D.speaker); free(D.text); memset(&D, 0, sizeof D); D.result = -3; ui_scale = 1; dbg = false;
}
