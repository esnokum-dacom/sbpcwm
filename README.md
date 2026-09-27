<div align="center">
  <h1>sbpcwm — experimental fork (Warning AI!)</h1>
  <p>
    <b>EXPERIMENTAL</b> — a fork of <a href="https://github.com/esnokum-dacom/sbcwm">sbcwm</a>,
    a lightweight canvas-based window manager written in <b>XCB</b>, inspired by
    <a href="https://github.com/esnokum-dacom/SOWM-Plus-Plus">SOWM++</a>.
  </p>
</div>

---

## Status: experimental

This is a playground fork. Expect rough edges, breaking changes and weird behavior.
The whole point of this fork is to turn window management into a **physics sandbox**:

- **Windows collide with each other.** Elastic AABB collisions with mass proportional to
  window area, restitution and positional correction — throw one window into a stack and
  watch the chain reaction. Only managed clients take part; override-redirect windows,
  docks, menus and fullscreen windows are never simulated.
- **Throw your windows.** Release a drag while moving and the window keeps its momentum,
  glides, bounces off other windows and settles by friction. While you drag, your window
  acts as an infinite-mass body that shoves everything else out of the way.
- **New windows never spawn on top of others.** Placement starts at the center of the
  pointer's monitor and spirals outwards — first inside the monitor, then across the
  whole canvas — until a free rectangle is found.
- **No walls.** Windows can travel to any part of the infinite canvas, including outside
  the visible monitor. Collisions are resolved per monitor, so a window flying over
  another monitor's territory does not interact with that monitor's windows.
- Physics runs on a fixed tick inside the main event loop (~60 fps) and sleeps completely
  when nothing is moving.

All tuning knobs live at the top of [`sbpcwm.h`](sbpcwm.h):

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
- **Titlebars** with close / maximize buttons (drawn via Xft)
- **Desktop icon shortcuts** — spawn apps from icons on the canvas
- **Right-click context menu** with your own entries
- **`sbpcwmctl`** — socket-based control client for runtime options, config reload and
  icon management
- **`sbpcs`** — graphical settings panel for everything above

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
git clone https://github.com/esnokum-dacom/sbpcwm
cd sbpcwm
sudo make clean install
```

That installs three programs:

| Program     | What it is                                            |
| ----------- | ----------------------------------------------------- |
| `sbpcwm`    | the window manager                                    |
| `sbpcwmctl` | text control client (options, reload, icons)          |
| `sbpcs`     | settings panel launcher, opens the panel in the WM    |

`make install` also drops a sample `config.lua` in `~/.config/sbcwm/`, unless you
already have one there — an existing config is never overwritten. Both window
managers and their clients read and write the same state, so the config is shared:

```
~/.config/sbcwm/config.lua   # options, keybinds, context menu, default icons
~/.config/sbcwm/icons.lua    # icon positions, written by the wm and sbpcs
```

### Runtime configuration (config.lua)

```lua
-- ~/.config/sbcwm/config.lua
defaultsh = "/bin/sh"

fonts = "Terminus:style=Regular:pixelsize=16:antialias=false"
fontb  = "FiraMonoNerdFont:style=Regular:pixelsize=20:antialias=false"

opts = {
  pan_step      = 120,     -- canvas pan distance per step
  titlebar      = 0,       -- enable/disable titlebars
  focus_follow  = 0,       -- focus follows the pointer
  border        = 1,       -- draw window borders
  border_width  = 1,
  ctxbg         = "#151515",
  ctxborder     = "#2d4d66",
  deco          = "#2d4d66",
}

-- right-click context menu entries
ctx = {
  { label = "Terminal",  func = "run", arg = {"st"} },
  { label = "Icons",     func = "toggle_icons" },
}

-- default desktop icons (positions are then saved in icons.lua)
icons = {
  { name = "Term", image = "/home/you/.config/sbcwm/icons/terminal.png",
    x = 80, y = 80, cmd = {"st"} },
}

-- keybinds; "super" is Mod4, "alt" is Mod1
keys = {
  { mod = {"super", "shift"}, key = "c",     func = "win_kill" },
  { mod = {"super"},          key = "f",     func = "win_fs" },
  { mod = {"super", "shift"}, key = "Left",  func = "canvas_pan_key", arg = 0 },
}

-- plain command bindings, no function involved
shortcuts = {
  { mod = {"super"},          key = "space", arg = {"lm"} },
}
```

Available `func` names: `run`, `quit`, `win_kill`, `win_center`, `win_fs`,
`canvas_pan_key`, `canvas_reset`, `move_nextmon`, `ws_focusnext`, `toggle_icons`,
`reload_config`. Unknown names are reported on stderr and the binding is ignored.
Modifier names are `super`, `alt`, `ctrl` and `shift`.

### Physics options

Physics is compile-time only (it needs to touch the client structs directly).
Edit the `PHYS_*` defines in `sbpcwm.h` and recompile. Set `PHYS_ENABLED` to `0`
to get plain sbcwm behavior back.

### Runtime options with sbpcwmctl

```sh
sbpcwmctl get <option>              # read a runtime option
sbpcwmctl set <option> <value>      # write a runtime option
sbpcwmctl reload                    # reload config.lua
sbpcwmctl options                   # list runtime options
sbpcwmctl settings                  # open the sbpcs settings panel
sbpcwmctl shortcut add <name> <image> <x> <y> <cmd...>
sbpcwmctl shortcut del <name>
sbpcwmctl shortcut move <name> <x> <y>
sbpcwmctl shortcut list
sbpcwmctl shortcut show on|off
```

`sbpcwmctl` is the text twin of the panel below: both edit the same options and
write them straight back to `config.lua`.

### Settings panel (sbpcs)

`sbpcs` opens a settings window drawn by the window manager itself. It edits the
options that live in `config.lua`: pick a group in the navigation column on the
left, then change values on the right.

```sh
sbpcs                 # open the settings panel
sbpcwmctl settings    # same thing
```

- **checkboxes** for on/off options (`titlebar`, `focus_follow`, `border`),
  **text fields** for numbers and strings
- pages for the keybind, context-menu and icon lists, where rows can be added,
  edited and deleted
- click a field and type; `Enter` applies, `Tab` goes to the next field,
  `Escape` cancels the edit, `BackSpace`/`Delete`/`Left`/`Right`/`Home`/`End`
  work as usual
- every change is applied at once and written back to
  `~/.config/sbcwm/config.lua`, so it survives a restart
- `config.lua` is edited in place: only the lines of changed options are
  rewritten, so spacing and options the panel does not know are kept
- the last navigation entry, **Other**, lists any option that is not named by a
  group, so a new option in `config.lua` shows up here without code changes
- `Escape` closes the panel; reopening it while it is already open just raises it

#### Which window manager sbpcs talks to

The panel is drawn by whichever window manager is running, so the client asks
`sbpcwm` first and falls back to `sbcwm` if that is the one running. It works on
both because the config and icon state is the same `~/.config/sbcwm` pair.

`sbpcwm` listens on `$XDG_RUNTIME_DIR/sbpcwm-0.sock` for `DISPLAY=:0` (or
`/tmp/sbpcwm-$UID-0.sock` without `XDG_RUNTIME_DIR`; a screen suffix in
`DISPLAY` becomes an underscore), so the two window managers never fight over
the same socket. `sbpcs` is named differently from sbcwm's own `sbcs` on
purpose: installing this fork no longer overwrites it.

```
$ sbpcs
sbpcs: no running window manager found (tried /run/user/1000/sbpcwm-0.sock, /run/user/1000/sbcwm-0.sock)
```

### Colors

`deco` colors the decorations: the focused client border and its titlebar,
while unfocused clients get a dimmed version of it. When `deco` is missing it
falls back to `ctxborder`. Colors are always drawn opaque: an alpha byte in the
hex value is ignored, and because a client border is drawn by the X server in the
client's own visual, the pixel is built for that visual with every alpha bit set,
so a 32-bit ARGB client cannot end up with a transparent border under a
compositor. Changing `ctxbg`, `ctxborder` or `deco` from `sbpcwmctl` or `sbpcs`
takes effect immediately; the values are cached when the config is read rather
than re-resolved per redraw. Toggling `border` or `border_width` is safe to do
with clients open: the width is owned by sbpcwm and is never taken from a client
request.

A window that reaches past a monitor edge is clipped to the part covered by a
monitor, so a window dragged across a seam between two monitors stays fully
visible. Only the dead space in a gap between monitors is cut away, which is what
panning the canvas reveals.

## Mouse bindings

| Combination           | Action                                                    |
| --------------------- | ---------------------------------------------------------- |
| `Mouse`               | focus under cursor                                         |
| `Left Mouse` drag     | move window — release mid-motion to *throw* it             |
| `Right Mouse` drag    | resize window                                              |
| `Mouse wheel` (Press) | move the canvas with the mouse position                    |

Dragging one window into another pushes the other window away.

## Keyboard bindings

There are no hardcoded bindings: everything comes from `config.lua`, and the
shipped file ships with

| Combination                | Action                |
| -------------------------- | --------------------- |
| `Super` + `Shift` + `c`    | kill window           |
| `Super` + `f`              | fullscreen toggle     |
| `Super` + `Shift` + `Left` | pan canvas left       |
| `Super` + `space`          | run `lm` (launcher)   |

`Super` is `Mod4`, `alt` is `Mod1`, so the usual `alt` + `Shift` + arrows panning
is just a few lines in your config. `canvas_pan_key` takes `0` left, `1` right,
`2` up, `3` down:

```lua
keys = {
  { mod = {"alt", "shift"}, key = "Left",   func = "canvas_pan_key", arg = 0 },
  { mod = {"alt", "shift"}, key = "Right",  func = "canvas_pan_key", arg = 1 },
  { mod = {"alt", "shift"}, key = "Up",     func = "canvas_pan_key", arg = 2 },
  { mod = {"alt", "shift"}, key = "Down",   func = "canvas_pan_key", arg = 3 },
  { mod = {"alt", "shift"}, key = "period", func = "move_nextmon" },
  { mod = {"alt", "shift"}, key = "Return", func = "canvas_reset" },
  { mod = {"alt"},          key = "c",      func = "win_center" },
}
```

Rebind anything without touching the source, then `sbpcwmctl reload` — or let
`sbpcs` do it for you.

---

*Upstream: [sbcwm](https://github.com/esnokum-dacom/sbcwm) — all credit for the base
window manager goes there. Everything physics-related here is experimental fork work.*
