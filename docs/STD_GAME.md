# std.game — 2D + 3D game engine

```kc
import std.game;
```

A complete, dependency-free game engine written in C and driven from KingCobra: software-rendered
2D and 3D, a **chunk-streamed world**, an **advanced HUD**, model loaders (**OBJ, glTF/GLB, FBX,
Unreal PSK/PSKX, STL, PLY**) and a library of **built-in default models**. It runs on X11, Win32 and a
**headless backend** (no display needed — CI, servers, tests, screenshots).

Heavy work (meshing, rasterising, streaming, physics, HUD) is native; scripts only decide *what*
to do each frame.

```kc
import std.game;

fn update(dt) {
    let md = game.mouse_delta();
    game.camera(8.0, 60.0, 8.0, 0.0 - md[0], 0.0 - md[1]);
}
fn main() {
    game.window("hello", 800, 450);
    game.world("", "gen", "nature", "seed", 7, "radius", 8);   // streamed 3D world
    game.hud("crosshair");
    game.run(update, null, 0);                                   // update(dt) every frame
}
```

Examples: `examples/game/explorer3d.kc` (walk, jump, break/place blocks, minimap, hotbar, dialog),
`platformer2d.kc` (streamed tilemap, animation, particles), `model_gallery.kc` (all built-in models).
Regression suite: `tests/game_test.kc` (headless).

## Conventions

| | |
|---|---|
| Handedness / axes | right-handed, **+Y up**, camera looks down **-Z** at yaw 0; yaw grows turning left |
| Units | 1 unit = 1 metre = 1 block |
| Angles | degrees (`camera`, `yaw`, `rot`), radians only in `game.sin/cos/atan2…` |
| Colours | `0xRRGGBB` (opaque), `0xAARRGGBB`, `"#rgb"`, `"#rrggbb"`, `"#rrggbbaa"`, names (`"red"`, `"sky"`, `"transparent"`…), `[r,g,b,a]`; **any negative number = transparent**; `game.rgb(r,g,b,a)` |
| Options | trailing `"name", value` pairs — `game.sprite(t, x, y, "scale", 2, "flip_x", true)` (or one options object) |
| Texture UV | origin top-left |

KingCobra has no math builtins beyond `sqrt`, so std.game provides `sin cos tan asin acos atan2 dsin dcos
sqrt abs floor ceil round pow min max clamp lerp smoothstep sign rad deg`.

## Frame loop

`game.run(update, draw, max_frames)` calls `update(dt)`, renders sky + world + entities + particles,
calls `draw()` (your immediate-mode 2D/3D drawing), draws the HUD and presents. Manual loop:

```kc
while (game.poll()) {          // events, time, input edges, entity physics, HUD tweens
    ...update...
    game.render3d();           // streams + draws the world (skip in 2D)
    ...immediate drawing...
    game.present();            // HUD + show
}
```

`window(title, [w, h], [scale])` · `title` · `size` · `headless([on])` · `is_headless` · `run` · `poll` · `begin` ·
`render3d` · `present` · `quit` · `dt` · `time` · `fps` · `frame` · `target_fps(n)` · `mode("2d"|"3d")` ·
`hud_auto(on)` · `advance(frames,[dt])` (run frames with no callbacks) · `screenshot(path)` (.png/.bmp/.ppm) ·
`pixel(x,y)` · `stats()` · `budget(model_mb, texture_mb)` · `reset()`.

`scale` renders at `window/scale` and upscales (cheap retro look / big speed-up).
If no display is available the engine falls back to headless with a warning.

## Input

`key(name)` `key_pressed` `key_released` · `mouse()` `mouse_delta()` `mouse_down([btn])` `mouse_pressed` `mouse_released` ·
`wheel()` · `capture_mouse(on)` (hidden cursor, relative look) · `typed()` · inject for tests: `send_key(name, down)`,
`send_mouse(x, y, [btn, down])`. Key names: letters, digits, `space enter escape tab backspace delete
insert left right up down home end pageup pagedown shift ctrl alt f1..f12 comma period slash minus equals
semicolon quote lbracket rbracket backslash grave`. Mouse buttons: `1/"left"`, `2/"middle"`, `3/"right"`.

## 3D: streamed world

The world is an unbounded grid of 16×16-column chunks (height 32–256, default 64). **The whole game is never
in memory:** every frame only chunks inside `radius` around the camera exist.

* A chunk comes from its file in the world directory (if edited/baked) or from the deterministic generator.
* Chunks outside `radius + keep` are saved (if edited) and freed; `max_chunks` caps memory.
* Loading and meshing are budgeted per frame (`loads_per_frame`, `meshes_per_frame` + ~6/10 ms wall time),
  nearest and in-front chunks first, so a fast flight never stalls.
* Meshes are built only for loaded chunks, hidden-face culled with ambient occlusion; drawing is frustum-,
  back-face- and fog-distance-culled (`stats()["chunks_culled"]`).
* **Unloaded areas are hidden automatically** — see *Fog / cloud* below.

```kc
game.world("saves/island", "gen", "mountains", "seed", 42, "radius", 10, "height", 96);
```

Generators: `nature` (default: continents, mountains, beaches, snow, forests, pines), `hills`, `mountains`,
`islands`, `flat`, `empty` (load-only world — pair with `world_bake`). The directory holds `world.kcw`
(seed/height/generator — reopening restores them) and `c.<cx>.<cz>.kcc` chunk files (run-length encoded).
Only **edited** chunks are written, so a world directory stays tiny; `world_bake(r)` writes every chunk within
`r` of the camera so the world becomes pure disk-streamed data. `""` as directory = memory only
(edited chunks are then never evicted).

`world([dir], options…)` · `world_close()` · `world_config(key, v)` (`radius keep max_chunks loads_per_frame
meshes_per_frame trees`) · `world_flush()` · `world_bake(radius)` · `block(x,y,z)` (-1 if not loaded) ·
`set_block(x,y,z, id|name)` · `block_define(name, top_tex, [side], [bottom], [solid], [translucent])` · `block_id` ·
`block_name` · `surface(x,z)` · `raycast(ox,oy,oz, dx,dy,dz, [max])` → `{x,y,z,nx,ny,nz,block,name,dist}` or null ·
`collide_move(x,y,z, half_w, height, dx,dy,dz, [step_up])` → `[x,y,z, grounded, hit_x, hit_y, hit_z]`.

Default blocks: grass dirt stone sand water log leaves snow planks brick (textures generated at start;
leaves are alpha-cutout, water is translucent). Custom blocks take any textures (resampled to 16×16).
`collide_move` sub-steps so nothing tunnels; unloaded chunks count as solid, so entities never fall into
unloaded ground.

### Fog / cloud (hides what is not loaded)

`fog("off"|"linear"|"exp"|"cloud", [start], [end], [colour|"auto"])` · `fog_auto(on)` · `sky(top,[horizon])` ·
`sky_clouds(on)` · `sun(on)` · `light(dx,dy,dz,[colour],[ambient])`.

With `fog_auto` (default) the fog end is **clamped to the nearest chunk that is still loading inside the view
cone**, and never exceeds the streaming radius. Closing in is fast, opening up is slow, so terrain never
pops in view — it appears out of the mist. `"cloud"` (default) animates a noise-modulated cloud bank and tints
toward white; the colour follows the sky horizon unless you pass one. Chunks wholly beyond the fog are not drawn.

## 3D: models, entities, drawing

Built-in default models (`game.models()`): cube sphere cylinder cone pyramid plane quad torus capsule stairs arch
pine tree bush rock house tower fence lamp crate barrel flower mushroom humanoid car cloud gem arrow coin campfire sign.
Use them by name anywhere a model is expected: `game.spawn("house", x,y,z)`, `game.draw_model("tree", …)`.

**Loading files** — `model(path_or_name, …)`, `model_load(path, options…)` (lazy: parsed on first draw, evicted
LRU above `budget()`, reloaded on demand; `"lazy", false` parses now and reports errors immediately).
Options: `scale`, `unreal`, `meters` (default true), `center`, `ground`, `flip_uv`, `flip_winding`, `tint`, `name`.

| Format | Support |
|---|---|
| `.obj` + `.mtl` | polygons, normals/UV, materials (`Kd`, `d`, `map_Kd`), negative indices |
| `.gltf` / `.glb` | glTF 2.0: node hierarchy (matrix/TRS), meshes, indices, normals, UV0, vertex colours, base-colour factor + texture (external, base64, GLB-embedded), alphaMode, doubleSided, unlit; Draco/meshopt are rejected with a message |
| `.fbx` | **binary 7.x** (zlib arrays) **and ASCII**; model transforms and parenting, per-polygon materials, normals/UV mappings, diffuse colour + texture; **up/front/coord axes and unit scale are read from the file** (Unreal's Z-up, cm → Y-up metres) |
| `.psk` / `.pskx` | **Unreal ActorX** static/skeletal mesh: points, wedges (16- and 32-bit faces), materials → `<material>.png/.tga/.bmp` next to the file; UE axes (X fwd, Y right, Z up, left-handed) and centimetres are converted automatically |
| `.stl` `.ply` | binary + ASCII STL; ASCII/binary PLY with vertex colours |
| `.kcm` | KingCobra custom model text format (below) |

**Unreal support, honestly:** Unreal's interchange formats (FBX, glTF, PSK/PSKX, OBJ) load directly with correct
coordinate and unit conversion (`"unreal", true` applies it to OBJ/STL/PLY too). Cooked `.uasset` / `.umap` packages are
version-locked, usually compressed and often encrypted, so they are **not** parsed — `model_load` stops with a message
telling you to export from the editor (Asset Actions → Export → FBX/glTF) or with UModel/ActorX to `.pskx`. Skeletal
meshes load in their bind pose (no skinning/animation yet). Textures: PNG, BMP, TGA, PPM (JPEG/DDS: convert first).

**Custom models.** Text format, usable from files or strings (`model_from_text(text, [name])`):

```
part color=#40a0ff [tex=file.png] [twosided] [unlit] [translucent]
v x y z [u v] [color=#rrggbb]      f a b c [d ...]      (0-based vertex indices, CCW = front)
box sx sy sz  cyl r0 r1 h  cone r h  sphere rx ry rz  plane w d  torus R r   [at=x,y,z] [rot=x,y,z] [seg=n] [color=..]
```

or build in code: `model_begin()`, `model_vertex(x,y,z,[u,v],[colour],[nx,ny,nz])`, `model_tri`, `model_quad`,
`model_part("tex", t, "color", c, "twosided", true …)`, `model_end([name])`. `model_save(model, "x.kcm")` writes any
model back out. Also `model_info(m)`, `model_texture(m, tex)`, `model_tint`, `model_free`, `model_formats()`.

**Entities** (retained, culled by distance + frustum, only simulated when their chunk is loaded):
`spawn(model, x,y,z, options…)` → id · `despawn` · `set(id, prop, value…)` · `get(id, prop)` · `alive` · `entity_count` ·
`move(id, dx,dy,dz)` · `pick([sx,sy],[max])` · `gravity(g)`. Properties: `pos x y z rot yaw pitch roll scale vel vx vy vz tint
visible gravity collide wire radius height spin tex tag model` (`grounded` read-only).
**Immediate**: `draw_model(model, x,y,z, "yaw", d, "scale", s, "tint", c, "tex", t, "wire", true)` ·
`billboard(tex, x,y,z, w,h)` · `line3d` · `box3d` · `emit(x,y,z,count, …)` particles · camera: `camera(x,y,z,[yaw],[pitch],[roll])` ·
`camera_get` · `look_at` · `camera_forward/right` · `fov` · `clip_planes` · `world_to_screen` · `screen_ray`.

## 2D

`clear rect rect_grad line circle triangle pixel_set text text_width text_height clip` ·
`texture(path)` (lazy, LRU) · `texture_new` · `texture_proc(kind,size,…)` (checker noise grass dirt stone sand brick wood planks water
metal snow leaves gradient stripes dots) · `texture_size/get/set/free/save` ·
`sprite(tex, x, y, "scale", s, "rot", deg, "flip_x", true, "tint", c, "alpha", a, "world", true, "center", true, "sx","sy","sw","sh", …)` ·
`sheet(tex, cw, ch, [margin], [spacing])`, `sheet_draw(sheet, index, x, y, …)` · `anim(sheet, [frames], fps, [loop])`, `anim_draw`, `anim_ctl("play"|"pause"|"stop"|"reset")` ·
`cam2d(x, y, [zoom])` (+ `"world", true` on sprites) · `emit2d`.

**Streamed tilemaps** — `tilemap(tw, th, sheet, [dir])`: 32×32-tile chunks created on write; with `dir` they are saved/loaded on demand
and evicted when off-screen. `tile_set/get/fill`, `tilemap_draw(map)` (touches only visible chunks), `tile_solid(map, tile, bool)`,
`tile_hit(map, x,y,w,h)`, `tile_move(map, x,y,w,h, dx,dy)` → `[x, y, hit_x, hit_y]` (sub-stepped), `tile_import(map, "level.csv", [ox],[oy])`
(streams line by line), `tilemap_flush`. Tile `0` = empty, `n` = sheet index `n-1`.

## HUD

Retained elements, drawn after the scene every frame (`hud_auto`). `id = game.hud(kind, …)`:

| kind | arguments → options |
|---|---|
| `"text"` | `anchor, x, y, text` → `color scale shadow bg` |
| `"bar"` | `anchor, x, y, w, h, value, max` → `color bg border trail numbers label vertical segments` (damage trail + smooth fill) |
| `"icon"` | `anchor, x, y, texture` → `w h tint` |
| `"panel"` | `anchor, x, y, w, h` → `color border radius title` |
| `"button"` | `anchor, x, y, w, h, text, [callback(id, arg)]` |
| `"crosshair"` | `["cross"|"dot"|"circle"|"plus"]` → `size gap color` |
| `"hotbar"` | `anchor, x, y, slots, slot_size` ; set items with `hud_set(id, "item", [slot, tex, count])`; clicks select |
| `"minimap"` | `anchor, x, y, size` → `scale rotate shape("circle") border` — samples the streamed chunks |
| `"compass"` | `anchor, x, y, width` |
| `"label3d"` | `x, y, z, text` → `color maxdist scale` (world-anchored, distance fade) |

Anchors: `top-left top top-right left center right bottom-left bottom bottom-right` (offsets push inward).
`hud_set(id, prop, v)` (`text value max visible x y w h color bg border alpha scale selected item on_click anchor z tex label zoom shadow title`),
`hud_get` · `hud_remove` · `hud_clear` · `hud_tween(id, prop, target, secs, ["linear"|"in"|"out"|"inout"|"back"])` (x y w h alpha value) ·
`hud_notify(text,[colour],[secs])` toasts · `hud_float(x,y,z,text,…)` rising damage numbers · `hud_dialog(speaker, text, [choices])` typewriter box
(space/enter/click advance, 1–9 or click choose; `dialog_result()` → `-2` open, `-1` closed, `≥0` choice) · `hud_debug([on])` (fps, position, chunks, triangles, memory)
· `hud_scale(n)` · `hud_at(x,y)`. HUD clicks are consumed before `mouse_pressed`.

## Performance notes

Rasteriser: perspective-correct, nearest-sampled, 1/w depth buffer, per-vertex lighting, per-pixel fog. A 640×360
frame with ~30k visible quads takes ~15–25 ms on one core. Levers, biggest first: `window(…, scale)` 2, lower
`radius` (fog end shrinks to match), `fog("linear")` instead of `"cloud"`, fewer entities/particles. Textures and
file models are lazy and evicted LRU above `budget(model_mb, texture_mb)` (default 256/256).

## Limits and what is not here yet

No audio, no skeletal animation/skinning, no JPEG/DDS, no shadows, no GPU path. `.uasset` is not parsed (see above).
X11 and headless are verified (incl. ASAN/UBSAN); the Win32 backend is written to the same interface but has not been run on Windows yet.
