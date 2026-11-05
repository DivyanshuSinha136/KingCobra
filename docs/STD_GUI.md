# std.gui — building GUI applications

```kingcobra
import std.gui;
```

A retained-mode widget toolkit with its own software renderer. Every widget
is laid out and drawn into a pixel buffer by this module (built-in 8×8 bitmap
font, themes, anti-alias-free crisp shapes), so apps look and behave the same
everywhere. Only "show the pixels / receive input" is platform code:

| Platform | Backend | Status |
|---|---|---|
| Linux | Xlib (compiled in when `<X11/Xlib.h>` exists; `make` detects it) | **Tested** under Xvfb with real injected mouse/keyboard events |
| Windows | Win32 + GDI (`-lgdi32 -luser32` in the Makefile) | Written against the documented API; **not yet run on a real Windows machine** |
| Headless | none — renders to memory | **Tested.** Automatic when there is no display, or `KC_GUI_HEADLESS=1`, or `gui.headless(true)` |

Headless mode makes every GUI app testable in CI: `gui.screenshot("x.bmp")`,
`gui.click(id)`, `gui.send_text(...)`, `gui.send_key(...)`, `gui.advance(ms)`.

## Model

One window; widgets are referenced by **integer ids**; `0` is the window's root
container. Every `gui.label(parent, ...)`-style call returns the new id.
Callbacks are plain KingCobra functions.

```kingcobra
import std.gui;
var n = 0;
fn bump(id) { n += 1; gui.set(1, "text", "clicked " + n); }

fn main() {
    gui.window("Hello", 400, 200);
    gui.label(0, "clicked 0");
    gui.button(0, "Click me", bump);
    gui.run();
}
```

## Windows, loop, timers

`window(title,w,h)` · `title(t)` · `size()` · `theme("dark"|"light")` · `scale(1..4)`
(font scale, default 2) · `run()` (blocks until closed or `quit()`) ·
`poll()` (one non-blocking step for your own loop; false once closed) ·
`update()` · `quit()` · `invalidate()` · `timer(ms,fn[,repeat])` · `after(ms,fn)` ·
`cancel_timer(id)` · `now()` · `headless(on)` · `is_headless()`.

## Containers and layout

`vbox(parent[,title])` · `hbox(parent[,title])` · `grid(parent,cols[,title])` ·
`absolute(parent[,title])` + `place(id,x,y[,w,h])` · `tabs(parent,names[,cb])` +
`page(tabs,i)` · `spacer(parent)` · `separator(parent)`.
A title makes a framed group box. Layout is automatic: children get their natural
size; `set(id,"weight",n)` makes a child absorb leftover space along the main axis
(in a grid, per column/row); `set(id,"align","start|center|end|fill")` overrides
cross-axis alignment; `width`/`height` force a size; `padding`/`spacing` tune gaps.

## Widgets

| Constructor | Primary callback |
|---|---|
| `label(parent,text)` | — |
| `button(parent,text[,on_click(id)])` | click |
| `checkbox(parent,text[,checked[,on_change(id,checked)]])` | change |
| `radio(parent,group,text[,checked[,on_change]])` | change (one checked per group per container) |
| `textbox(parent[,text[,on_change(id,text)]])` | change; `submit(id,text)` on Enter |
| `slider(parent,min,max,value[,on_change(id,value)])` | change |
| `progress(parent,fraction)` | — |
| `list(parent,items[,on_select(id,index)])` | select; `activate` on double-click/Enter |
| `dropdown(parent,items[,selected[,on_change(id,index)]])` | change/select |
| `canvas(parent,w,h[,on_draw(id,w,h)])` | draw |

Text boxes support caret movement, Home/End, Shift-selection, mouse selection,
Ctrl+A/C/X/V (internal clipboard), `password`, `readonly`, `placeholder`.
Keyboard: Tab/Shift+Tab focus, Space/Enter activate, arrows on sliders/lists/
dropdowns/tabs. Mouse wheel scrolls lists.

## Properties and events

`set(id, prop, value)` / `get(id, prop)`: `text value checked selected items
enabled visible min max step weight width height x y padding spacing cols scale
color bg password readonly placeholder align text_align` (+ read-only `count
selected_text focused caret selection`). Colors are `0xRRGGBB` ints or `"#rrggbb"`.
`on(id, event, fn)` events: `click change select submit activate draw key
mouse_down mouse_up mouse_move wheel resize close` (use id `0` for window-level
key/mouse/resize/close; a `close` callback returning `false` cancels closing).
Also: `add_item remove_item remove clear(container) focus bounds children
widget_at clipboard set_clipboard`.

## Canvas drawing (only inside a draw callback)

`clear(color)` `rect(x,y,w,h,color[,fill])` `round_rect` `line(x1,y1,x2,y2,color[,thickness])`
`circle(cx,cy,r,color[,fill])` `triangle` `pixel` `text(x,y,str,color[,scale])`
`text_size(str[,scale])`. Coordinates are relative to the canvas. Call
`gui.invalidate()` from event handlers to request a redraw.

## Dialogs

`alert(title,text[,cb])` · `confirm(title,text,cb(bool))` ·
`dialog(title,text,buttons[,cb(index)])` — modal; Enter/Esc/arrows work (Esc
passes index `-1`).

## Known limits (v0.4)

* One window per program; no menu bar, multi-line text area, scroll containers or images yet (see ROADMAP).
* Text is rendered with an 8×8 ASCII font; other characters draw as `?`. Typed
  non-ASCII input is accepted by text boxes but displays as `?`.
* Software rendering of the whole window each frame — fine for tools and
  games-UI scale windows, not a replacement for a GPU toolkit.
* Windows backend untested on real Windows (see table above).

Examples: `examples/std_gui.kc` (headless, asserted in `make test`),
`examples/gui/todo_app.kc`, `examples/gui/paint.kc` (interactive).
