# std.ascii_screen showcase

Run from the repository root (a real console: Windows Terminal, PowerShell, cmd.exe — not Git Bash/mintty). `q` quits every one.

| Example | Shows | Keys |
|---|---|---|
| `show_effects.kc` | all 10 effects (plasma fire matrix starfield rain snow waves tunnel life noise) | space/←/→ |
| `show_text.kc` | big fonts, gradients, rainbow, shadow, 8 box styles, colour ramps, text attributes, Unicode, wrapping | space/←/→ |
| `show_hires.kc` | braille / quad / half sub-cell graphics: live plot, Lissajous, spinning polygon | space = mode |
| `show_images.kc` | JPEG/SVG/animated GIF → text in 7 render modes and 3 dithers | space = mode, d = dither |
| `show_fireworks.kc` | particles, gravity, colour fade, camera shake | click / space |
| `show_hud.kc` | panel, tweened bars, list, input box, live graph, spinner, toasts | ↑↓ enter, tab |
| `show_world.kc` | infinite streamed terrain, smooth camera, distance fog | WASD, f, n |
| `show_transitions.kc` | 11 scene transitions between two live effects | space |
| `show_pathfinding.kc` | dungeon generation, A*, field of view | space, r, f |
| `show_dashboard.kc` | responsive system-monitor TUI (gauges, sparklines, braille history, table) | — |
| `probe.kc`, `probe2.kc` | step-by-step terminal/feature checks; last printed step names a crash | — |

`diag.kc` shows what the terminal supports (backend, keys, colour, glyphs).
