<div align="center">
  <h1>sbcwm — experimental fork (Warning AI!)</h1>
  <p>
    <b>EXPERIMENTAL</b> — a fork of <a href="https://github.com/esnokum-dacom/sbpcwm">sbcwm</a>,
    a lightweight canvas-based window manager written in <b>XCB</b>, inspired by
    <a href="https://github.com/esnokum-dacom/SOWM-Plus-Plus">SOWM++</a>.
  </p>
  <p>
    <img src="sbcwm.png" width="59%" align="center">
  </p>
</div>

---

## Status: experimental

This is a playground fork. Expect rough edges, breaking changes and weird behavior.
The whole point of this fork is to turn window management into a **physics sandbox**:

- **Windows collide with each other.** Elastic AABB collisions with mass proportional to
  window area, restitution, positional correction — throw one window into a stack and
  watch the chain reaction. Only managed clients participate; override-redirect windows,
  docks, menus and fullscreen windows are never simulated.
- **Throw your windows.** Release a drag while moving and the window keeps its momentum,
  glides, bounces off other windows and settles by friction. While you drag, your window
  acts as an infinite-mass body that shoves everything else out of the way.
- **New windows never spawn on top of others.** Placement starts at the center of the
  pointer's monitor and spirals outwards — first inside the monitor, then across the
  whole canvas — until a free rectangle is found.
- **No walls.** Windows can travel to any part of the infinite canvas, including outside
  the visible monitor. Collisions are resolved per-monitor, so a window flying over
  another monitor's territory does not interact with that monitor's windows.
- Physics runs on a fixed tick inside the main event loop (~60 fps) and sleeps completely
  when nothing is moving.

All tuning knobs live at the top of [`sbcwm.h`](sbcwm.h):

```c
#define PHYS_ENABLED      1     /* master switch                          */
#define PHYS_RESTITUTION  0.55f /* bounciness of window-window hits       */
#define PHYS_FRICTION     4.0f  /* exponential damping per second         */
#define PHYS_STOP_SPEED   8.0f  /* px/s below which a window sleeps       */
#define PHYS_VMAX         3200.f/* velocity clamp                         */
#define THROW_WINDOW_MS   120   /* pointer history used for flicks        */
#define THROW_BOOST       1.15f /* flick speed multiplier                 */
```

## What is inherited from sbcwm?

A stacking window manager built directly on **XCB (X C Binding)** instead of Xlib,
with a few extras plain sowm doesn't have:

- **Lua runtime configuration** (`config.lua`) — reloadable while running
- **Canvas panning** — an infinite desktop you can move around with keyboard or mouse
- **Minimap** — a live overview of your canvas
- **Titlebars** with close / maximize buttons (drawn via Xft)
- **Desktop icon shortcuts** — spawn apps from icons on the canvas
- **Right-click context menu** with your own entries
- **`sbcwmctl`** — socket-based control client for runtime options, config reload and
  icon/shortcut management

## Dependencies

- Xlib
- xcb
- Xinerama
- Xft
- xcb-randr
- xcb-shape
- xcb-icccm
- xcb-keysyms
- xcb-util
- lua5.3
- libpng16, libjpeg

## Install & Config

```sh
git clone https://github.com/esnokum-dacom/sbcwm
cd sbcwm
sudo make clean install
```

The wm reads your config from `~/.config/sbcwm/config.lua` (a copy is placed there by
`make install`). You can tweak the running instance without recompiling.

### Runtime configuration (config.lua)

```lua
-- ~/.config/sbcwm/config.lua
fonts = "Terminus:style=Regular:pixelsize=16:antialias=false"

opts = {
  pan_step      = 120,     -- canvas pan distance per step
  titlebar      = 0,       -- enable/disable titlebars
  ui            = 1,       -- show the minimap/HUD
  xr_colors     = 1,       -- use Xresources colors
  border        = 1,       -- draw window borders
  border_width  = 1,
  ctxbg         = "#151515",
  ctxborder     = "#2d4d66",
}

-- right-click context menu entries
ctx = {
  { label = "Terminal",  func = "run", arg = {"st"} },
  { label = "Icons",     func = "toggle_icons" },
}

keys = {
  { mod = {"super", "shift"}, key = "c",     func = "win_kill" },
  { mod = {"super"},          key = "f",     func = "win_fs" },
}
```

### Physics options

Physics is compile-time only (it needs to touch the client structs directly).
Edit the `PHYS_*` defines in `sbcwm.h` and recompile. Set `PHYS_ENABLED` to `0`
to get plain sbcwm behavior back.

#### Runtime options with sbcwmctl

```sh
sbcwmctl get <option>              # read a runtime option
sbcwmctl set <option> <value>      # write a runtime option
sbcwmctl reload                    # reload config.lua
sbcwmctl options                   # list runtime options
sbcwmctl shortcut add <name> <image> <x> <y> <cmd...>
sbcwmctl shortcut del <name>
sbcwmctl shortcut move <name> <x> <y>
sbcwmctl shortcut list
sbcwmctl shortcut show on|off
```

## Mouse bindings

| Combination           | Action                                                    |
| --------------------- | ---------------------------------------------------------- |
| `Mouse`               | focus under cursor                                         |
| `Left Mouse` drag     | move window — release mid-motion to *throw* it             |
| `Right Mouse` drag    | resize window                                              |
| `Mouse wheel` (Press) | move the canvas with the mouse position                    |

Dragging one window into another pushes the other window away.

## Keyboard bindings

| Combination                      | Action                       |
| -------------------------------- | ---------------------------- |
| `MOD1` + `f`                     | maximize toggle              |
| `MOD1` + `c`                     | center window                |
| `MOD1` + `Shift` + `c`           | kill window                  |
| `MOD1` + `TAB` (*alt-tab*)       | focus cycle                  |
| `MOD1` + `Shift` + `Left/Right`  | pan canvas left / right      |
| `MOD1` + `Shift` + `Up/Down`     | pan canvas up / down         |
| `MOD1` + `b`                     | toggle minimap               |

> Keybindings come from `config.lua` — the defaults above are just what's shipped.
> Rebind anything without touching the source.

---

*Upstream: [sbcwm](https://github.com/esnokum-dacom/sbpcwm) — all credit for the base
window manager goes there. Everything physics-related here is experimental fork work.*
