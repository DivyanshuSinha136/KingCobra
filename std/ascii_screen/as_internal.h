/* KingCobra std.ascii_screen — internal shared header.
 *
 * std.ascii_screen is a native terminal (TUI) screen + ASCII-art engine. Everything is plain C
 * with no third-party libraries. Files:
 *
 *   as_util.c    argument/option parsing, colours, UTF-8, noise/RNG, text-width table
 *   as_term.c    terminal backends (POSIX / Windows console / headless), live key + mouse input,
 *                diff-based frame output, main loop timing
 *   as_cell.c    cell canvases, text, shapes, boxes, hi-res (half/quad/braille) pixel layers, text export
 *   as_image.c   image store + decoders (PNG/BMP/TGA/PPM via std.game, GIF, JPEG, ffmpeg fallback),
 *                resampling and the image -> ASCII renderers
 *   as_svg.c     SVG parser + anti-aliased rasteriser (-> image -> ASCII)
 *   as_font.c    built-in pixel fonts, FIGlet (.flf) and BDF loaders, user-defined fonts
 *   as_anim.c    animations (GIF / frame sequences / video via ffmpeg / .kca), procedural effects,
 *                transitions
 *   as_hud.c     retained-mode HUD / widget layer
 *   as_game.c    ASCII game engine: sprites, tile maps, streamed worlds, entities, particles,
 *                camera, controller bindings, raycaster
 *   as_api.c     KingCobra bindings
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#ifndef KC_ASCII_SCREEN_INTERNAL_H
#define KC_ASCII_SCREEN_INTERNAL_H

#include "interp.h"

/* std.game exports some of the same names; give ours a private prefix so both link into one binary */
#define A_argc as_A_argc
#define A_bool as_A_bool
#define A_col as_A_col
#define A_int as_A_int
#define A_num as_A_num
#define A_str as_A_str
#define V_isnum as_V_isnum
#define V_num as_V_num
#define anim_ctl as_anim_ctl
#define anim_new as_anim_new
#define col_from_value as_col_from_value
#define col_hsv as_col_hsv
#define col_mix as_col_mix
#define col_scale as_col_scale
#define ent_alive as_ent_alive
#define ent_despawn as_ent_despawn
#define ent_get as_ent_get
#define ent_move as_ent_move
#define ent_set as_ent_set
#define ent_spawn as_ent_spawn
#define hud_dialog_result as_hud_dialog_result
#define hud_draw as_hud_draw
#define hud_remove as_hud_remove
#define hud_reset as_hud_reset
#define hud_tween as_hud_tween
#define hud_update as_hud_update
#define key_code as_key_code
#define key_name as_key_name
#define opt_bool as_opt_bool
#define opt_col as_opt_col
#define opt_find as_opt_find
#define opt_has as_opt_has
#define opt_int as_opt_int
#define opt_num as_opt_num
#define opt_str as_opt_str
#define opts_make as_opts_make
#define sprite_draw as_sprite_draw

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ colour
 * Col: 0 = unset (terminal default / "keep"), otherwise COL_SET | 0xRRGGBB. */
typedef uint32_t Col;
#define COL_SET 0x01000000u
#define COL_R(c) ((int)(((c) >> 16) & 255))
#define COL_G(c) ((int)(((c) >> 8) & 255))
#define COL_B(c) ((int)((c) & 255))
static inline int as_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float as_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline Col col_rgb(int r, int g, int b) { return COL_SET | ((uint32_t)as_clampi(r, 0, 255) << 16) | ((uint32_t)as_clampi(g, 0, 255) << 8) | (uint32_t)as_clampi(b, 0, 255); }
Col  col_mix(Col a, Col b, float t);
Col  col_scale(Col a, float k);
Col  col_hsv(float h, float s, float v);
float col_luma(Col c);                                  /* 0..1 perceptual luminance */
Col  col_from_ansi(int idx);                            /* 0..255 xterm palette -> colour */
int  col_to_256(int r, int g, int b);
int  col_to_16(int r, int g, int b);                    /* 0..15 */
int  as_lerpi(int a, int b, float t);

/* ------------------------------------------------------------------ cells */
enum { AT_BOLD = 1, AT_DIM = 2, AT_ITALIC = 4, AT_UNDER = 8, AT_BLINK = 16, AT_REVERSE = 32, AT_STRIKE = 64 };
#define CH_CONT 0u                  /* second half of a double-width character */
typedef struct { uint32_t ch; Col fg, bg; uint16_t attr; } Cell;
typedef struct { Col fg, bg; uint16_t attr; } Style;

typedef struct { int w, h; Cell *c; int cx0, cy0, cx1, cy1; bool used; } Canvas;   /* clip rect [cx0,cx1) x [cy0,cy1) */

/* ------------------------------------------------------------------ errors / args */
typedef struct { KcInterp *ip; KcPos pos; } AsCtx;
extern AsCtx AC;
void as_err(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void as_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void A_argc(int n, int lo, int hi, const char *sig);
bool   V_isnum(KcValue v);
double V_num(KcValue v, const char *who);
double A_num(KcValue *a, int n, int i, const char *who);
int    A_int(KcValue *a, int n, int i, const char *who);
const char *A_str(KcValue *a, int n, int i, const char *who);
bool   A_bool(KcValue *a, int n, int i, const char *who);
Col    col_from_value(KcValue v, const char *who);
Col    A_col(KcValue *a, int n, int i, const char *who);
bool   col_parse(const char *s, Col *out);

typedef struct { KcValue *a; int n, start; bool has_obj; KcValue obj; } Opts;
Opts  opts_make(KcValue *a, int n, int start);
const KcValue *opt_find(const Opts *o, const char *key);
bool  opt_has(const Opts *o, const char *key);
double opt_num(const Opts *o, const char *key, double def);
int   opt_int(const Opts *o, const char *key, int def);
bool  opt_bool(const Opts *o, const char *key, bool def);
Col   opt_col(const Opts *o, const char *key, Col def);
const char *opt_str(const Opts *o, const char *key, const char *def);

KcValue ret_str(const char *s, size_t len);
KcValue ret_cstr(const char *s);
bool as_is_fn(KcValue v);
bool as_file_exists(const char *p);
uint8_t *as_read_file(const char *path, size_t *len);        /* NULL on failure; caller frees */
bool as_write_file(const char *path, const void *d, size_t n);

/* ------------------------------------------------------------------ growable byte buffer */
typedef struct { char *d; size_t n, cap; } Buf;
void buf_init(Buf *b);
void buf_add(Buf *b, const void *p, size_t n);
void buf_adds(Buf *b, const char *s);
void buf_addc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_free(Buf *b);
/* emit the SGR sequence that moves the terminal from (cf,cb,ca) to (fg,bg,attr); cmode 1 = 16, 2 = 256, 3 = truecolor */
void sgr_apply(Buf *b, int cmode, Col fg, Col bg, uint16_t attr, Col *cf, Col *cb, uint16_t *ca);

/* ------------------------------------------------------------------ utf-8 / width */
uint32_t utf8_next(const char **p);                          /* decodes one code point, advances; invalid -> U+FFFD */
int  utf8_put(char *out, uint32_t cp);                       /* returns bytes written (1..4) */
int  cp_width(uint32_t cp);                                  /* 0 combining, 1 normal, 2 wide */
int  str_width(const char *s);

/* ------------------------------------------------------------------ rng / noise / time */
void   as_seed(uint32_t s);
uint32_t as_rand_u32(void);
float  as_rand(void);                                        /* [0,1) */
float  as_noise2(float x, float y, uint32_t seed);           /* value noise, 0..1 */
float  as_fbm(float x, float y, uint32_t seed, int oct);
uint32_t as_hash2(int x, int y, uint32_t seed);
double as_clock_s(void);
void   as_sleep_ms(int ms);

/* ------------------------------------------------------------------ canvases (as_cell.c) */
Canvas *cv_get(int id);                                      /* NULL if invalid */
Canvas *cv_must(int id, const char *who);
int    cv_new(int w, int h);
void   cv_free(int id);
void   cv_resize(Canvas *c, int w, int h);
void   cv_clear(Canvas *c, uint32_t ch, Col fg, Col bg, uint16_t attr);
Cell  *cv_at(Canvas *c, int x, int y);                       /* NULL if outside the canvas */
void   cv_put(Canvas *c, int x, int y, uint32_t ch, Col fg, Col bg, uint16_t attr);   /* clipped; bg==0 keeps the cell's bg */
void   cv_set_clip(Canvas *c, int x, int y, int w, int h);
void   cv_reset_clip(Canvas *c);
int    cv_text(Canvas *c, int x, int y, const char *s, size_t len, Col fg, Col bg, uint16_t attr);  /* returns cells advanced; no newline handling */
void   cv_fill(Canvas *c, int x, int y, int w, int h, uint32_t ch, Col fg, Col bg, uint16_t attr);
void   cv_blit(Canvas *dst, Canvas *src, int sx, int sy, int sw, int sh, int dx, int dy, bool skip_space, bool skip_empty_bg);
char  *cv_to_text(Canvas *c, int x0, int y0, int w, int h, int color_mode, bool trim, size_t *len);   /* malloc'd; color_mode 0 = plain text, 1 ansi16, 2 ansi256, 3 truecolor */
char  *cv_to_html(Canvas *c, size_t *len);
extern int   S_target;                                       /* current draw target canvas id (0 = screen back buffer) */
extern Style S_style;
Canvas *cv_target(void);
const char *box_chars(const char *style);                    /* 8 chars as 8 utf-8 strings: tl,tr,bl,br,h,v,... see as_cell.c */
void   draw_box(Canvas *c, int x, int y, int w, int h, const char *style, Col fg, Col bg, uint16_t attr, bool fill, const char *title, Col title_fg);
void   draw_line(Canvas *c, int x0, int y0, int x1, int y1, uint32_t ch, Col fg, Col bg, uint16_t attr);
void   draw_ellipse(Canvas *c, int cx, int cy, int rx, int ry, uint32_t ch, Col fg, Col bg, uint16_t attr, bool fill);
void   draw_triangle(Canvas *c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t ch, Col fg, Col bg, uint16_t attr);
void   draw_flood(Canvas *c, int x, int y, uint32_t ch, Col fg, Col bg, uint16_t attr);
int    cv_wrap_lines(const char *s, int width, char ***lines_out);        /* word-wrap; returns line count, caller frees lines + array */

/* hi-res pixel layers: half (1x2), quad (2x2), braille (2x4) sub-pixels per cell */
typedef enum { HR_HALF = 0, HR_QUAD = 1, HR_BRAILLE = 2 } HrMode;
typedef struct { int cw, ch, w, h; HrMode mode; Col *px; bool used; } Hires;   /* px: pixel colours, 0 = off */
Hires *hr_get(int id);
int    hr_new(int cw, int ch, HrMode mode);
void   hr_free(int id);
void   hr_draw(Hires *h, Canvas *c, int x, int y, Col bg, uint16_t attr);
bool   hr_mode_parse(const char *s, HrMode *m);
void   hr_to_cells(const Col *px, int pw, int ph, HrMode mode, Canvas *c, int x, int y, Col bg, uint16_t attr);

/* ------------------------------------------------------------------ terminal (as_term.c) */
typedef struct {
    bool open, headless, raw, quit, resized, sigint_quit, alt_screen, mouse_on, kitty, kitty_supported, focused;
    int  w, h;                      /* terminal size in cells */
    int  colors;                    /* 0 none, 16, 256, 24 truecolor */
    const char *backend;            /* "posix" | "windows" | "headless" */
    const char *keymode;            /* "kitty" | "windows" | "timed" | "virtual" */
    double dt, time, fps, target_fps, last_real;
    int64_t frame;
    double hold_first, hold_repeat; /* timed key-hold fallback (seconds) */
    bool ctrl_c_quits, cursor_visible, title_set;
    double cell_aspect;             /* cell height / width (default 2.0) */
    int mx, my, dmx, dmy, wheel_y, wheel_x, mods;
    char typed[256];
    int last_key;
    int hw, hh;                     /* headless virtual size */
    uint64_t bytes_out;             /* bytes written to the terminal (stats) */
    int cells_changed;
    int cur_x, cur_y; bool cur_show;
    double dt_override;
} Term;
extern Term T;
#define KEYN 256
int   key_code(const char *name);
const char *key_name(int code);
bool  term_open(KcInterp *ip, Opts *o);
void  term_close(void);
void  term_restore_for_error(void);
bool  term_poll(void);                         /* events + timing; false once quit was requested */
void  term_present(void);                      /* diff the back buffer onto the terminal (HUD is drawn by the caller) */
void  term_frame_wait(void);                   /* sleep to honour target_fps */
void  term_set_title(const char *s);
void  term_refresh(void);                      /* force a full redraw next present */
void  term_bell(void);
bool  term_key(int c);  bool term_key_pressed(int c);  bool term_key_released(int c);  bool term_key_repeat(int c);
bool  term_mouse_down(int b);  bool term_mouse_pressed(int b);  bool term_mouse_released(int b);
void  term_send_key(int code, bool down, int mods);
void  term_send_key_tap(int code, int mods);
void  term_send_mouse(int x, int y, int button, int kind);   /* kind 0 move, 1 press, 2 release, 3 wheel(button = +-1) */
void  term_send_text(const char *s);
void  term_advance(double dt);
void  term_resize_virtual(int w, int h);
extern Canvas *S_screen;                       /* the screen's back buffer (canvas 0) */
void  term_ensure(void);
int   term_mods(void);

/* ------------------------------------------------------------------ images (as_image.c) */
typedef struct { int w, h; uint32_t *px; bool used; int nframes; int *delays_ms; uint32_t **frames; } Image;   /* px 0xAARRGGBB; frames: optional animation (GIF) */
Image *im_get(int id);
Image *im_must(int id, const char *who);
int    im_new(int w, int h, uint32_t fill);
void   im_free(int id);
int    im_load(const char *path, int svg_w, int svg_h);           /* any supported format; may call ffmpeg; svg rasterised at svg_w x svg_h (0 = natural) */
int    im_from_rgba(int w, int h, uint32_t *px);                   /* takes ownership */
Image *im_resize_box(const Image *src, int dw, int dh);           /* area-average; malloc'd Image (px owned), not registered */
void   im_release(Image *im);
bool   gif_decode(const uint8_t *d, size_t n, Image *out, char *err);
bool   jpeg_decode(const uint8_t *d, size_t n, Image *out, char *err);
bool   ext_decode_image(const char *path, Image *out, char *err);  /* ffmpeg fallback */
bool   ffmpeg_available(void);
bool   ffmpeg_probe(const char *path, int *w, int *h, double *fps, double *duration);
FILE  *ffmpeg_open_frames(const char *path, int w, int h, double fps, double start, const char *pix_fmt, bool loop);
void   ffmpeg_close(FILE *f);
bool   svg_render(const char *src, size_t n, int w, int h, Image *out, char *err);  /* as_svg.c */

typedef struct {
    int  width, height;            /* cells; 0 = auto */
    const char *mode;              /* ramp | shape | half | quad | braille | edges | blocks */
    const char *ramp;              /* characters dark -> light */
    int  color;                    /* 0 none, 1 fg, 2 fg+bg */
    const char *fit;               /* contain | cover | stretch */
    const char *dither;            /* none | floyd | bayer */
    float contrast, brightness, gamma, threshold, sharpen, aspect;
    bool invert, transparent, light_bg;
    Col  matte;
    int  edge_min;                 /* sobel threshold 0..255 */
    bool edge_fill;
    float saturation;
    bool quantize_ansi;            /* snap colours to the terminal palette depth now */
} RenderOpts;
void render_opts_default(RenderOpts *r);
void render_opts_parse(RenderOpts *r, const Opts *o);
int  render_image_to_canvas(const Image *im, const RenderOpts *r, int *out_w, int *out_h);   /* returns new canvas id */
float *im_resample(const Image *src, int cx, int cy, int cw, int ch, int dw, int dh);
void render_dims(const Image *im, const RenderOpts *r, int *W, int *H);
bool im_save(const Image *im, const char *path, char *err);
void render_image_into(const Image *im, const RenderOpts *r, Canvas *dst, int x, int y);    /* draws at x,y, size from r->width/height */
const char *ramp_preset(const char *name);
void glyph_mask_5x7(uint32_t ch, uint8_t rows[7]);              /* 5x7 font glyph rows (bit4 = leftmost) */
extern const char *RAMP_NAMES[];

/* ------------------------------------------------------------------ fonts (as_font.c) */
int  font_find(KcValue v, const char *who);                      /* id or name (builtin/loaded) */
int  font_load_file(const char *path, const char *name);
int  font_define(const char *name, int h, const char *chars, KcValue rows);
void font_free_id(int id);
int  font_by_name(const char *n);
typedef struct { int w, h; char **lines; } BigText;               /* lines: utf-8 rows */
void big_text_render(int font, const char *s, const Opts *o, Canvas *c, int x, int y, bool draw, int *out_w, int *out_h);
KcValue big_text_lines(int font, const char *s, const Opts *o);
void fonts_free_all(void);
extern const char *FONT_BUILTIN_NAMES[];

/* ------------------------------------------------------------------ animations / effects (as_anim.c) */
int  cv_from_text(const char *s, size_t n, bool cp437, int wrap);   /* text / ANSI art -> new canvas id */
int  anim_new(double fps);
bool anim_valid(int id);
void anim_free(int id);
void anims_update_frame(double dt, int64_t frame);
void anims_free_all(void);
void anim_add_canvas(int id, Canvas *c, int delay_ms);
bool anim_save_file(int id, const char *path);
int  anim_load_file(const char *path, const Opts *o);
void anim_info(int id, int *w, int *h, int *n, double *fps, int *cur, bool *playing, bool *finished, bool *stream);
void anim_ctl(int id, const char *op, double v);
void anim_draw_into(int id, Canvas *dst, int x, int y, bool transparent);
Canvas *anim_frame_canvas(int id, int frame, Canvas *tmp);
bool effect_valid(int id);
void effects_free_all(void);
int  effect_new(const char *name, int w, int h, const Opts *o);
void effect_step(int id, double dt);
void effect_draw(int id, Canvas *c, int x, int y, Col tint);
void effect_free(int id);
void transition_draw(const char *kind, Canvas *a, Canvas *b, Canvas *dst, float t, int x, int y);

/* ------------------------------------------------------------------ HUD (as_hud.c) */
int  hud_create(const char *kind, const Opts *o);
void hud_set_prop(int id, const char *prop, KcValue v);
KcValue hud_get_prop(int id, const char *prop);
void hud_remove(int id);
void hud_focus_set(int id);
int  hud_focus_get(void);
void hud_push(int id, double v);
void hud_tween(int id, const char *prop, double to, double secs, const char *ease);
void hud_dialog_start(const char *speaker, const char *text, KcValue choices, Col fg, double speed);
int  hud_dialog_result(void);
bool hud_dialog_finished(void);
bool hud_input_focused(void);
int  hud_at(int x, int y);
int  game_entity_count(void);
void hud_update(float dt);
void hud_draw(Canvas *c);
void hud_reset(void);
bool hud_active_dialog(void);
bool hud_handle_mouse_click(int x, int y, int button);
bool hud_handle_key(int code, const char *typed, int mods);
void hud_pre_poll(void);
void hud_notify_text(const char *s, Col fg, float secs);
void hud_set_debug(bool on);
void hud_set_enabled(bool on);

/* ------------------------------------------------------------------ tile maps / worlds (as_map.c) */
typedef struct { uint32_t ch; Col fg, bg; bool solid, opaque, defined; int flags; char name[20]; } TileDef;
typedef struct { int vx, vy, vw, vh; double camx, camy; int xscale; float fog; bool fov; Col fog_col; } MapDraw;
TileDef *tile_def_get(int id);
void tile_define(int id, uint32_t ch, Col fg, Col bg, bool solid, bool opaque, int flags, const char *name);
int  tile_id_of(KcValue v, const char *who);                      /* int or 1-char string */
int  map_new_fixed(int w, int h, int fill);
int  map_new_world(const char *gen, uint32_t seed, int radius);
int  map_from_rows(KcValue rows);
bool map_valid(int m);
void map_free(int m);
int  map_tile(int m, int x, int y);
void map_set(int m, int x, int y, int t);
bool map_solid(int m, int x, int y);
bool map_opaque(int m, int x, int y);
bool map_size(int m, int *w, int *h);                             /* false for infinite worlds */
bool map_spawn(int m, int *x, int *y);
void map_focus(int m, double x, double y);
void map_stats(int m, int *loaded, int *modified, bool *infinite);
void map_generate(int m, const char *kind, uint32_t seed);
void map_fill_rect(int m, int x, int y, int w, int h, int t);
void map_draw(int m, Canvas *c, const MapDraw *d);
void map_fov(int m, double x, double y, int radius);
bool map_los(int m, int x0, int y0, int x1, int y1);
int  map_path(int m, int x0, int y0, int x1, int y1, int **xy);   /* returns node count, caller frees */
void map_update(double dt);
void maps_free_all(void);
extern int G_map;                                                  /* active map used for collisions */
extern int G_xscale;
typedef struct { double dist; int tile, side; double hx, hy; } RayHit;
bool map_raycast(int m, double x, double y, double ang, double maxd, RayHit *out);
void raycast_view(int m, Canvas *c, double px, double py, double ang, double fov_deg, int vx, int vy, int vw, int vh, const Opts *o);

/* ------------------------------------------------------------------ game engine (as_game.c) */
int  sprite_create(KcValue text, const Opts *o);
int  sprite_create_anim(KcValue frames, double fps, const Opts *o);
int  sprite_from_canvas(Canvas *c, double fps);
void sprite_free(int id);
bool sprite_valid(int id);
void sprite_size(int id, int *w, int *h, int *n);
void sprite_set_anchor(int id, int ax, int ay);
void sprite_draw(int id, Canvas *c, int x, int y, int frame, bool fx, bool fy, Col tint, double t0);
Cell *sprite_cells(int id, int frame, int *w, int *h);
int  ent_spawn(double x, double y, const Opts *o);
void ent_despawn(int id);
bool ent_alive(int id);
void ent_set(int id, const char *key, KcValue v);
KcValue ent_get(int id, const char *key);
bool ent_move(int id, double dx, double dy);
KcValue ent_query(int mode, double x, double y, double w, double h, const char *tag, int self);   /* 0 at-box, 1 within radius (w), 2 all */
KcValue ent_touching(int id, const char *tag);
void ent_clear(void);
void emit_particles(double x, double y, const Opts *o);
void camera_set(double x, double y);
void camera_follow(int id, double smooth, double dz_w, double dz_h);
void camera_shake(double amt, double secs);
void camera_bounds(int m);
void camera_get(double *x, double *y);
void viewport_set(int x, int y, int w, int h);
void viewport_get(int *x, int *y, int *w, int *h);
void world_to_screen(double wx, double wy, double *sx, double *sy);
void screen_to_world(double sx, double sy, double *wx, double *wy);
void game_physics(double gravity, double drag);
void game_draw_world(Canvas *c, const Opts *o);
void bind_action(const char *name, KcValue *keys, int n);
void unbind_action(const char *name);
void bind_axis(const char *name, const char *neg, const char *pos);
void controller_preset(const char *preset);
bool action_state(const char *name, int mode);                   /* 0 down 1 pressed 2 released 3 repeat */
double axis_value(const char *name);
KcValue actions_list(void);
void game_update(float dt);
void game_reset(void);
extern double G_cam_x, G_cam_y;

/* module table */
extern KcModuleEntry AS_ENTRIES_A[]; extern const int AS_ENTRIES_A_N;

#endif
