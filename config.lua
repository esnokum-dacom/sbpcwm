defaultsh = "/bin/sh"

fonts = "Terminus:style=Regular:pixelsize=16:antialias=false"
fontb = "FiraMonoNerdFont:style=Regular:pixelsize=20:antialias=false"

opts = {
  pan_step = 120,
  titlebar = 0,
  focus_follow = 0,
  border = 1,
  border_width = 1,
  ctxbg = "#151515",
  ctxborder = "#2d4d66",
  deco = "#2d4d66",
}

ctx = {
  { label = "Terminal",  func = "run", arg = {"st"} },
  { label = "Launcher",  func = "run", arg = {"lm"} },
  { label = "Wallpaper", func = "run", arg = {"xwall"} },
  { label = "Icons",     func = "toggle_icons" },
}

icons = {
  { name = "Term", image = os.getenv("HOME") .. "/.config/sbcwm/icons/terminal.png", x = 80, y = 80, cmd = {"st"} },
}

keys = {
  { mod = {"super", "shift"}, key = "c",     func = "win_kill" },
  { mod = {"super"},          key = "f",     func = "win_fs" },
  { mod = {"super"},          key = "space", func = "run", arg = {"lm"} },
  { mod = {"super","shift"},  key = "Left",  func = "canvas_pan_key", arg = 0 },
}
