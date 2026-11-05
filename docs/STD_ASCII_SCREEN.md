# std.ascii_screen

A native **terminal (TUI) screen and ASCII-art engine** for KingCobra. Plain C, no third-party libraries.

```kc
import std.ascii_screen;

fn main() {
    ascii_screen.open();
    ascii_screen.run(fn(dt) {
        ascii_screen.box(0, 0, 30, 5, "style", "round", "title", "Hello", "fg", "cyan");
        ascii_screen.big_text(2, 1, "Hi!", "font", "half", "gradient", ["#ff8a00", "#7b2ff7"]);
        if (ascii_screen.key_pressed("q")) { ascii_screen.quit(); }
    });
    ascii_screen.close();
}
```

(`kingcobra run` calls `main()` for you.) Example programs live in `examples/ascii_screen/`
(`viewer.kc`, `gallery.kc`, `roguelike.kc`, `platformer.kc`, `raycaster.kc`, plus the ten `show_*.kc` feature showcases: effects, text, hires, images, fireworks, hud, world, transitions, pathfinding, dashboard; `probe.kc`/`probe2.kc` check a terminal step by step); the regression suite is
`tests/ascii_screen_test.kc` (headless, 61 checks).

## Contents

1. [Backends, colour and keyboard modes](#backends)
2. [Screen lifecycle and main loop](#lifecycle)
3. [Drawing: styles, text, shapes, canvases, hi-res layers](#drawing)
4. [Images, SVG → ASCII](#images)
5. [ASCII animations and video](#animations)
6. [Fonts and big text](#fonts)
7. [Effects and transitions](#effects)
8. [Live input, controller and mouse](#input)
9. [HUD widgets, toasts, dialogs](#hud)
10. [ASCII game engine](#game)
11. [Math and colour helpers](#math)
12. [Testing headless, limits, source layout](#testing)

<a id="backends"></a>
## 1. Backends, colour and keyboard modes

| Backend | Where | Notes |
|---|---|---|
| `posix` | Linux / macOS / BSD terminals | termios raw mode, alternate screen, SGR mouse, bracketed paste, focus events, SIGWINCH resize, SIGTSTP/SIGCONT, SIGINT/SIGTERM restore |
| `windows` | Windows console (Windows Terminal, conhost on Windows 10+) | VT output + `ReadConsoleInput` (real key down/up, mouse, resize). *Compile-checked with mingw-w64, not run on a Windows machine in development.* |
| `headless` | no TTY (pipes, CI) or `"headless", true` / `KC_ASCII_HEADLESS=1` | no terminal needed; virtual size and time, injectable input, `dump()` returns the frame as text |

`open()` falls back to headless automatically when stdin/stdout is not a terminal (or `TERM=dumb`), so scripts and tests never write
escape codes into pipes. Pass `"force", true` to override.

**Colour** is detected (`NO_COLOR`, `COLORTERM`, `TERM`, `WT_SESSION`, …) as `truecolor`, `ansi256`, `ansi16` or `none`; every colour you
draw is stored as 24-bit RGB and quantised to the terminal's depth at output time. Override with `open("color", "ansi256")` or `color_mode(...)`.

**Live keys.** Terminals normally report only key *presses*. The module uses, in order:

| `caps()["keys"]` | Source | `key_released` / `key()` accuracy |
|---|---|---|
| `kitty` | kitty keyboard protocol (kitty, foot, WezTerm, Ghostty, Alacritty ≥ 0.13, iTerm2, …), auto-detected | exact press / repeat / release, modifier keys |
| `windows` | console input API | exact down / up |
| `timed` | any other terminal | a key counts as held for `hold_first` (0.40 s) after the first event, then `hold_repeat` (0.12 s) after each auto-repeat. A quick second tap while still "held" is seen as a repeat — use `key_repeat()` for step-wise movement |
| `virtual` | headless | exactly what you inject |

Tune the fallback with `key_hold(first, repeat)`.

The terminal is always restored (alternate screen left, cursor shown, raw mode off) on normal exit, `close()`, Ctrl+C, SIGTERM, Ctrl+Z
(and re-entered on `fg`) — and **before an error message is printed**, so script errors stay readable. (A small `kc_error_hook` in the core makes that possible.)

Output is **diff-rendered**: only changed cells are written, cursor moves are minimised, SGR changes are incremental, frames are wrapped in
synchronized-output sequences (`CSI ? 2026`) to avoid flicker, and auto-wrap is disabled so the bottom-right cell is safe.

<a id="lifecycle"></a>
## 2. Screen lifecycle and main loop

| Function | |
|---|---|
| `open(options...)` / `screen(...)` | open the screen. Options: `"headless"`, `"width"`, `"height"` (headless size), `"color"` (`auto none ansi16 ansi256 truecolor`), `"mouse"` (true), `"alt_screen"` (true), `"kitty"` (`"auto"`/`"on"`/`"off"`), `"fps"` (30), `"title"`, `"cell_aspect"` (2.0), `"ctrl_c"` (`"quit"` default, or `"key"` to receive Ctrl+C as a key), `"hold_first"`, `"hold_repeat"`, `"force"` |
| `close()`, `is_open()` | |
| `size()` → `[w,h]`, `width()`, `height()`, `resized()` | `size(w,h)` resizes a headless screen |
| `caps()` | `{backend, keys, color, kitty_keys, mouse, headless, ffmpeg, width, height, unicode}` |
| `title(text)`, `bell()`, `cursor(visible)` / `cursor(x,y)`, `refresh()`, `color_mode([mode])` | |
| `run(update [, draw [, max_frames]])` | the main loop. One function → `begin(); fn(dt); present()`. Two → `update(dt)`, then `begin()`, `draw()`, `present()`. Runs until `quit()`, Ctrl+C or `max_frames`. |
| `poll()`, `begin()`, `present()` | manual loop: `while (poll()) { begin(); …draw…; present(); }`. `poll` reads input, advances animations, HUD, and the game world |
| `quit()`, `should_quit()` | |
| `dt()`, `time()`, `fps()`, `frame()`, `target_fps(n)`, `advance(seconds)` | `advance` forces the next frame's dt (headless/deterministic tests) |
| `stats()` | `{fps, frame, dt, time, width, height, bytes_out, cells_changed, entities, backend, keys}` |
| `hud_auto(on)`, `game_auto(on)` | disable automatic HUD drawing / entity physics |
| `reset()` | free everything (entities, HUD, animations, effects, maps) |

<a id="drawing"></a>
## 3. Drawing

All drawing goes to the **current target**: the screen's back buffer by default, or any canvas selected with `target(id)` (`target(0)` = screen).
Cells hold a Unicode code point (double-width CJK/emoji handled), foreground, background and attributes.

**Options** are trailing `"name", value` pairs (or one object): `fg`, `bg`, `bold`, `dim`, `italic`, `underline`, `blink`, `reverse`, `strike`.
`style(options...)` sets defaults, `style()` resets. When `bg` is not given, text/shapes keep the cell's existing background.

**Colours** are `0xRRGGBB`, `"#rgb"`, `"#rrggbb"`, `[r,g,b]`, or a name: `black red green yellow blue magenta cyan white gray orange purple brown pink gold sky lime teal navy …`
and `bright_*`. `-1`/`"default"` means the terminal default.

| Function | |
|---|---|
| `clear([char], options...)`, `put(x,y,char,opts)`, `fill(x,y,w,h[,char],opts)`, `hline`, `vline` | |
| `text(x,y,string,opts)` | multi-line (`\n`); `"width"` wraps, `"align"` = `left center right`, `"pad"` fills the box. Returns the widest line |
| `text_width(s)`, `text_wrap(s, width)` | |
| `box(x,y,w,h, opts)` | `"style"`: `single double round bold ascii block dashed dots thick_half none`; `"title"`, `"fill"`, `"title_fg"` |
| `line(x0,y0,x1,y1[,char],opts)` | auto glyph `- | / \` when no char given |
| `circle(cx,cy,r,opts)`, `ellipse(cx,cy,rx,ry,opts)`, `triangle(...)`, `flood(x,y,char,opts)` | `circle` is aspect-corrected (`"aspect"`, default 2); `"fill"`, `"char"` |
| `clip(x,y,w,h)`, `unclip()` | |
| `canvas(w,h)`, `canvas_free`, `canvas_size`, `canvas_resize`, `canvas_copy`, `target([id])`, `blit(src,dx,dy[,sx,sy,sw,sh][,"transparent",true])`, `cell(x,y[,canvas])` | off-screen buffers |
| `dump([canvas])` | the frame as plain text (rows trimmed) |
| `dump_ansi([canvas[,mode]])` | with ANSI colour codes (`ansi16`, `ansi256`, `truecolor`) |
| `canvas_text(canvas[,x,y,w,h])`, `save_text(path[,canvas])`, `save_ansi(...)`, `save_html(path[,canvas])` | export |
| `canvas_load(path, "cp437", true, "wrap", 80)`, `canvas_from_text(str)` | import plain text / **ANSI art** (`.ans`, CP437, SGR colours, cursor moves, SAUCE stripped) |

**Hi-res layers** give sub-cell pixels: `hires(cell_w, cell_h, mode)` with `half` (1×2 px/cell), `quad` (2×2) or `braille` (2×4).
`hires_set/get/line/rect/circle/clear/size/draw`, and `plot(id, values, "color", c, "min", lo, "max", hi)` for charts.

<a id="images"></a>
## 4. Images and SVG → ASCII

| Format | Support |
|---|---|
| PNG, BMP, TGA, PPM/PGM | native |
| JPEG | native baseline decoder with EXIF orientation; progressive JPEG via ffmpeg |
| GIF | native (all frames, transparency, disposal, interlace) |
| **SVG** | native rasteriser: shapes, full path syntax, transforms, viewBox, strokes, linear/radial gradients, `<style>` rules, `use`/`defs`. `<text>`, filters, masks, clip paths and patterns are skipped (never an error) |
| WebP, AVIF, TIFF, … | via `ffmpeg` if installed |

```kc
let img = ascii_screen.image("photo.jpg");
print(ascii_screen.image_to_ascii(img, "width", 80, "mode", "shape"));        // plain string
ascii_screen.draw_image(img, 0, 0, "width", 60, "mode", "half", "dither", "floyd");   // onto the screen
let svg = ascii_screen.svg("logo.svg", "width", 800);                           // or inline "<svg …>"
```

`image(path[,"width",px,"height",px])`, `image_new`, `image_free`, `image_size`, `image_frames`, `image_get`, `image_set`, `image_save(path.png|bmp|ppm|tga)`,
`svg(path_or_markup)`, `image_to_ascii(img|path, opts)` → string, `image_to_canvas(...)` → canvas id, `draw_image(img|path, x, y, opts)`, `ramps()`.

**Render options** (all functions above): `width`, `height` (cells; one → the other follows the image aspect), `fit` (`contain cover stretch`),
`mode`, `ramp`, `color`, `ansi` (strings only), `dither`, `contrast`, `brightness`, `gamma`, `saturation`, `sharpen`, `invert`, `light_bg`,
`threshold` (`"auto"` = Otsu), `aspect` (cell height/width, 2.0), `matte` (colour under transparent pixels), `transparent`, `edge_min`, `edge_fill`.

| `mode` | What it does |
|---|---|
| `ramp` | luminance → character density. `ramp` = a preset (`classic detailed blocks minimal binary dots`) or your own string, sparse → dense |
| `blocks` | `ramp` with ` ░▒▓█` |
| `shape` | picks the glyph whose 5×7 shape best matches each cell — much crisper edges than density ramps (ASCII glyphs; `ramp` limits the candidate set) |
| `half` | `▀` half blocks, 2 px per cell (true-colour: effectively 2× vertical resolution) |
| `quad` | `▘▝▖▗▌▐▀▄▞▚▛▜▙▟█` quadrant blocks, 2×2 px per cell with a 2-colour split |
| `braille` | 2×4 dots per cell (finest monochrome detail) |
| `edges` | Sobel edge orientation `- / | \` (`edge_fill` shades the rest) |

`color`: `false`/`"none"` mono, `true`/`"fg"` coloured glyphs, `"bg"` also a dimmed background. `dither`: `none floyd bayer`.
Mono output uses the terminal's default foreground so light themes work (`light_bg` flips density).

<a id="animations"></a>
## 5. ASCII animations and video

`anim_load(path, options...)` accepts any render option plus `fps`, `max_frames` (3000), `start`, `duration`, `stream`:

| Source | |
|---|---|
| `.gif` | native, honours per-frame delays |
| `.kca` | KingCobra ASCII animation (run-length coded cells, colours kept) |
| `.txt` / `.ans` | frames separated by form-feed lines (`\f`); ANSI colour supported |
| `"frames/%04d.png"` | image sequence (any supported image format) |
| `.mp4 .mkv .webm .mov …` | video through **ffmpeg** (must be on `PATH`). `"stream", true` decodes on demand with constant memory (long videos); otherwise frames are rendered into memory |

Build your own: `anim_new(fps)`, `anim_add(anim, canvas[, delay_ms])`, `anim_from_text(frames[, fps])`, `anim_from_images(paths, options)`, `anim_frame(id, n)`.
Playback: `anim_draw(id, x, y["transparent",true])` (advances automatically once per frame), `anim_play`, `anim_pause`, `anim_stop`, `anim_seek(id, frame|seconds)`,
`anim_speed(id, k)`, `anim_loop(id, "once"|"loop"|"pingpong")`, `anim_info(id)` → `{width,height,frames,fps,frame,playing,finished,streaming}`, `anim_save(id, "x.kca"|"x.txt")`, `anim_free`.

<a id="fonts"></a>
## 6. Fonts and big text

`big_text(x, y, text, options...)` → `[w,h]`; also `big_text_size(text, opts)` and `big_text_lines(text, opts)` (array of strings).
Options: `font`, `fg`, `gradient` (`[c1,c2,…]`), `rainbow`, `align` (`left center right`), `scale` (1–8), `spacing`, `line_gap`, `shadow` (colour), `layout`
(FIGlet: `full fitted smush`), `style`/`char` (for loaded bitmap fonts).

| Font | |
|---|---|
| built-in `block` `half` `quad` `braille` `ascii` | the 5×7 pixel font drawn as full blocks (square pixels), half blocks, quadrants, braille dots, or a character per pixel |
| `font_load(path[, name])` | **FIGlet** `.flf`/`.tlf` (full width, fitted, and smushing with rules 1–6 / universal) or **BDF** bitmap fonts |
| `font_define(name, height, chars, glyphs)` | your own: `glyphs[i]` is an array of row strings, `#` = on |

`fonts()`, `font_free(id)`.

<a id="effects"></a>
## 7. Effects and transitions

`effect(name, w, h, options...)` — `plasma fire matrix starfield rain snow waves tunnel life noise`; options `speed`, `seed`, `ramp`, `color`, `fg`, `transparent`,
`count`, `density`/`rate` (life). `effect_draw(id, x, y)` advances once per frame automatically (`effect_step(id, dt)` to drive it yourself), `effect_free`.

`transition(kind, from_canvas, to_canvas, t[, x, y])` with `kind` = `fade dissolve wipe_left wipe_right wipe_up wipe_down iris curtain blinds slide_left slide_right`.

<a id="input"></a>
## 8. Live input, controller and mouse

Keys are lowercase names: letters, digits, `space enter escape tab backspace delete insert left right up down home end pageup pagedown shift ctrl alt super f1…f12`,
`comma period slash minus equals semicolon quote lbracket rbracket backslash grave`, or any single printable character.

| | |
|---|---|
| `key(k)` held · `key_pressed(k)` · `key_released(k)` · `key_repeat(k)` (press **or** auto-repeat, ideal for roguelike stepping) | |
| `keys_down()`, `last_key()`, `typed()` (text entered this frame, incl. paste), `mods()` → `{shift,alt,ctrl,super}` | |
| `mouse()` → `[col,row]`, `mouse_delta()`, `mouse_down/pressed/released(["left"|"middle"|"right"])`, `wheel()` | |
| `send_key(k[,down[,mods]])`, `send_mouse(x,y[,button[,"press"\|"release"\|"move"\|"wheel"]])`, `send_text(s)` | inject input (tests, demos); `send_key(k)` is a one-frame tap |

**Controller bindings** — actions instead of raw keys, for keyboard *and* mouse:

```kc
ascii_screen.controller("both");                       // arrows + WASD + hjkl; also confirm/cancel/pause/fire/jump/alt/inventory
ascii_screen.bind("dash", "shift", "mouse_right");     // "mouse_left" "mouse_middle" "mouse_right" "wheel_up" "wheel_down"
if (ascii_screen.action_pressed("dash")) { … }
let dx = ascii_screen.axis("move_x");                  // -1, 0, 1   (move_y: up = -1, down = +1)
ascii_screen.bind_axis("zoom", "zoom_out", "zoom_in");
```

`controller([preset])` (`arrows wasd vim both`), `bind`, `unbind`, `action`, `action_pressed`, `action_released`, `action_repeat`, `axis`, `bind_axis`, `actions()`,
`mouse_world()` (mouse position in world coordinates). Actions are inactive while a text input widget has focus.

<a id="hud"></a>
## 9. HUD widgets, toasts, dialogs

`hud(kind, options...)` → id. Kinds: `text panel bar list button input hotbar graph minimap spinner crosshair fps`.
Common options: `anchor` (`tl t tr l c r bl b br`), `x`, `y` (offset from the anchor), `w`, `h`, `z`, `visible`, `text`, `title`, `fg`, `bg`, `border`
(`single double round …`/`false`), `border_fg`, `align`, `bold`.

| Kind | Specifics |
|---|---|
| `bar` | `value`, `max`, `fill`, `empty`, `trail` (damage trail), `trail_color`, `vertical`, `text` label |
| `list` | `items`, `sel`, `on_select(index)`, `hover`; arrows/PageUp/Home/Enter, mouse click + wheel, scrollbar |
| `button` | `text`, `on_click()`; Enter/Space when focused or click |
| `input` | text field with cursor (`on_select(text)` on Enter, `hud_get(id,"text")`) |
| `hotbar` | `items`, `sel`, `on_select` |
| `graph` | `series`, `min`, `max_y`; push live data with `hud_push(id, v)` — braille line chart |
| `minimap` | `canvas` — any canvas you render |
| `panel`, `text` | multi-line text, optional border/title |

`hud_set(id, prop, value)`, `hud_get(id, prop)` (`text value max x y w h sel selected visible z count focused`), `hud_remove`, `hud_clear`, `hud_focus([id])` (Tab cycles),
`hud_at(x,y)`, `hud_tween(id, "x"|"y"|"w"|"h"|"value"|"max", target, seconds[, "linear"|"in"|"out"|"inout"])`, `hud_enable(on)`, `hud_debug([on])` (FPS/bytes/cells overlay).
`hud_notify(text, "fg", c, "seconds", 3)` stacked toasts. `hud_dialog(speaker, text[, choices][, "speed", 45])` is a typewriter dialog
(Enter/Space completes then advances, ↑/↓ or 1–9 choose); poll `dialog_active()` and `dialog_result()`.

<a id="game"></a>
## 10. ASCII game engine

**Sprites** — multi-line ASCII with colour: `sprite(rows, "fg", c, "colors", rows, "palette", "r=red,g=#00ff00", "transparent", " ")`,
`sprite_anim(frames, fps, opts)`, `sprite_from_image(img, opts)` (colour sprite from a picture), `sprite_draw(id,x,y,"frame",n,"flip_x",true,"fg",tint)`,
`sprite_size`, `sprite_anchor`, `sprite_free`. Flipping mirrors direction-sensitive glyphs (`/ \ ( ) < > ▌ …`).

**Tiles and maps**
- `tile_define(tile, glyph, "fg", c, "bg", c, "solid", true, "opaque", true, "name", s)`; a tile is an id 0–255 or a one-character string (`"#"` = 35). Sensible defaults exist for `# . + ~ " T` and the generator tiles.
- Fixed maps: `map_rows(rows)` (`@` = spawn), `map_new(w,h[,fill])`, `map_generate(map, "dungeon"|"cave"|"terrain"[, seed])`.
- **Streamed worlds**: `world(generator, "seed", n, "radius", chunks)` with `flat` (hills/dirt/ore side-view), `terrain` (biomes), `caves`, `noise`. Worlds are infinite, built from 32×32 chunks generated on demand from the seed;
  only chunks near the camera stay loaded (radius `+2` kept), **edited chunks are never lost**, everything else is regenerated identically — memory stays bounded however far you travel (`map_stats(map)` → `loaded_chunks`).
- `map_use(map)` (collisions, streaming, `draw_world`), `tile_at`, `tile_set`, `tile_solid`, `map_fill`, `map_size` (null for worlds), `map_spawn`, `map_stats`, `map_free`.
- `world_scale(n)`: screen cells per tile horizontally (2 makes tiles look square).
- `fov(map, x, y, radius)` + `draw_world("fov", true)` (explored areas stay dimmed on fixed maps), `los(map, x0,y0,x1,y1)`, `path(map, x0,y0,x1,y1)` (A*, 4-way, returns `[[x,y],…]`).
- Distance **fog**: `draw_world("fog", radius, "fog_color", c)`.

**Entities and physics** — `spawn(x, y, "glyph", "@", "sprite", id, "fg", c, "vx", v, "vy", v, "w", w, "h", h, "tag", s, "solid", true, "gravity", k, "bounce", b, "drag", d, "hp", n, "ttl", s, "z", n, "face", true, "billboard", true, "physics", true, "collide", true)`.
`set(id, prop, value)` / `get(id, prop)` (any name works; built-ins `x y vx vy w h hp tag z sprite visible solid on_ground ttl cx cy`), `move(id,dx,dy)` (grid-style step with collision), `despawn`, `alive`, `touching(id[,tag])`,
`entities_at(x,y[,w,h[,tag]])`, `entities_near(x,y,r[,tag])`, `entities([tag])`, `clear_entities()`. Entity ids carry a generation, so a stale id never touches a different entity.
`physics(gravity[, drag])` — tiles/s²; `0` = top-down. Movement is per-axis with tile and solid-entity collision.

**Particles** — `emit(x, y, "count", 10, "speed", 6, "spread", 360, "angle", 90, "life", 0.8, "gravity", 0, "glyphs", "*+.", "fg", c1, "fg2", c2, "screen", false)` (4096 max).

**Camera** — `camera(x,y)`, `camera_follow(id, "smooth", 8, "deadzone_w", w, "deadzone_h", h)`, `camera_shake(amount, seconds)`, `camera_bounds(map)`, `viewport(x,y,w,h)`,
`world_to_screen`, `screen_to_world`. `draw_world("map", id, "tiles", true, "entities", true, "particles", true, "fog", r, "fov", false)` draws map → entities (sorted by `z`, then `y`) → particles.

**Raycaster** (first-person ASCII 3D) — `raycast_view(map, x, y, angle, "fov", 70, "max_dist", 24, "fog", d, "ramp", s, "color", true, "ceiling", c, "floor", c, "sprites", true, "x","y","w","h")`
shades walls by distance and side, tints with the tile's colour, and draws entities as depth-tested billboards (set `"billboard", false` on the player). `raycast(map,x,y,angle[,max])` → `{dist,tile,x,y,side}` for gameplay.

<a id="math"></a>
## 11. Math and colour helpers

So scripts do not need another module: `sin cos tan asin acos atan atan2 dsin dcos sqrt abs floor ceil round sign pow min max clamp lerp smoothstep rad deg`,
`rand([max]|[lo,hi])`, `rand_int(lo,hi)`, `seed(n)`, `noise(x,y[,seed])`, `fbm(x,y[,octaves[,seed]])`, `rgb(r,g,b)`, `hsv(h,s,v)`, `mix(c1,c2,t)`, `shade(c,k)`.

<a id="testing"></a>
## 12. Testing, limits, source layout

- **Headless tests**: `open("headless", true, "width", 80, "height", 24)`, drive input with `send_key/send_mouse/send_text`, step with `poll()`, assert on `dump()`, `cell()`, `get()`. Time is virtual (`1/fps` per poll, or `advance(dt)`).
- `kingcobra run tests/ascii_screen_test.kc` — 61 checks; also run by `tests/smoke_test.sh`.
- **ffmpeg** is optional and only used for video, progressive JPEG and exotic image formats (`caps()["ffmpeg"]`).
- Scripts that must print to the terminal while the screen is open should draw text instead; raw `print` output would corrupt the display.
- Limits: canvases 4M cells, hi-res layers 2M cells, 65 535 live entities, 4096 particles, 96 actions, GIF 5000 frames, SVG raster ≤ 4096 px, wide characters rely on a built-in width table.
- Known gaps: no `<text>` in SVG, progressive JPEG needs ffmpeg, Windows backend untested on real hardware, no sound (use `bell()`), no gamepad.

```
std/ascii_screen/
  as_util.c      args, options, colours, UTF-8, width table, RNG, noise          as_term.c   terminal backends + input + diff output
  as_cell.c      canvases, shapes, boxes, hi-res layers, export                  as_image.c  image store, GIF/JPEG, ffmpeg, resampling
  as_render.c    image → ASCII renderers                                         as_svg.c    SVG rasteriser
  as_font.c      pixel/FIGlet/BDF fonts                                          as_anim.c   animations, video, effects, transitions, ANSI loader
  as_hud.c       widgets                                                         as_map.c    tiles, streamed worlds, FOV, A*, raycaster
  as_game.c      sprites, entities, physics, particles, camera, controller       as_api*.c   KingCobra bindings
```
