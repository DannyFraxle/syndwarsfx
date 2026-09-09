# SyndWarsFX 3D — Setup and Tuning Guide

`syndwarsfx3d.exe` is the Syndicate Wars fan port with a hardware-accelerated
(OpenGL) renderer bolted onto the original software engine. The game logic,
sprites, textures and palette are unchanged — the world is re-drawn as real 3D
geometry with a depth buffer, so it gains dynamic lights, shadows, ambient
occlusion, water reflections, transparency, weather and a decoupled frame rate,
while still looking like Syndicate Wars.

It is an **update to an existing installation**. It does not install the game and
contains none of the original game data.

---

## 1. What you need

* A working **Syndicate Wars fan port** installation (`syndwarsfx.exe` plus the
  `data`, `qdata`, `levels`, `sound`, `intro` and `music` folders). If you do not
  have one yet, install it first from your CD or GOG copy using the standard
  installer, then come back here.
* **Windows**, 32-bit or 64-bit.
* A GPU and driver supporting **OpenGL 3.3 core**. Anything from roughly 2010
  onwards qualifies (Intel HD 4000, GeForce 400, Radeon HD 5000 and newer).
  Onboard graphics will run it, but see *Performance* below.

---

## 2. Installing

### From the zip

1. **Back up your installation folder**, or at least `syndwarsfx.exe` and `conf/`.
   The renderer ships its own `conf/` files and will overwrite them.
2. Unzip the package **into your existing game folder** — the one that already
   contains `syndwarsfx.exe` and the `data` folder. Say yes when Windows asks to
   merge/replace.
3. Run **`syndwarsfx3d.exe`**.

Your original `syndwarsfx.exe` is untouched, so the software renderer stays
available; the two can live side by side.

> **If you have tuned `conf/fx3d.ini` yourself**, copy it somewhere safe before
> unzipping. The package ships a fresh one and will replace yours.

### From the installer

`syndwarsfx-fx3d-setup.exe` does the same thing with a wizard, and backs up your
old executable and `conf/` folder to `fx3d-backup-<version>/` first. It keeps an
existing `fx3d.ini` and drops the new defaults beside it as `fx3d.ini.new`.

---

## 3. First run

Launch `syndwarsfx3d.exe` and start a mission. You should see:

* real perspective on buildings, with the ground lit by street lamps,
* soft contact shading where objects meet the ground,
* a readout in the top-left corner reading
  `FPS <display rate>  TPS <turns/sec>  ms avg <n> worst <n>  BT <n>`.

`TPS` is the simulation rate and should sit at **16** — that is the game's native
turn rate and is deliberately fixed. `FPS` is the display rate and is free to run
much higher. `BT` is the bullet-time multiplier, `1.00` except during a slow-motion
dip.

If the window opens black or the game exits immediately, see *Troubleshooting*.

Useful command-line switches:

```
syndwarsfx3d.exe --windowed --hwrender-aa=0
```

`--hwrender-aa=N` sets the MSAA sample count, and
`--hwrender-filter-ground|-objects|-sprites=on|off` toggle texture filtering.
All of them override `fx3d.ini` for that run only.

---

## 4. Configuring — `conf/fx3d.ini`

**Every** renderer setting lives in `conf/fx3d.ini`, inside your game folder.
Edit it with any text editor and restart the game; nothing needs recompiling.
Lines starting with `;` are comments, and each section is documented in the file
itself.

The settings shipped in that file are the tuned defaults — the same values are
compiled into the executable, so if a key or the whole file goes missing the game
still looks the way it is meant to.

The sections, in the order they appear:

| Section | Controls |
| --- | --- |
| `[fx3d]` | Anti-aliasing, texture filtering, frame rate, VSync, FPS overlay |
| `[defaultlighting]` | Global light gain, ambient, tint, per-category lamp brightness and reach |
| `[sun]` | Optional directional sun and its shadow map (off by default) |
| `[firelight]` `[persuadelight]` | Flickering ground light from fires and from persuasion |
| `[floor]` `[water]` `[upscale]` | Ground tiles, water shine and reflection, texture upscaling |
| `[sprites]` `[transparency]` `[glare]` | Billboard scaling and filtering, see-through faces, headlamp/siren glow |
| `[ssao]` `[rain]` `[fog]` `[bullettime]` | Ambient occlusion, weather, slow-motion on explosions |
| `[thing_categories]` | Which lamp type each object counts as — written by the in-game panel |

### The settings people change most

* `[fx3d] AntiAliasing` — `0`, `2`, `4` or `8`. The single biggest quality/cost
  dial. `0` if the frame rate is poor.
* `[fx3d] TargetFPS` — `0` means uncapped. Set a number to cap the display rate.
* `[fx3d] ShowFPS` — set to `False` once you have finished checking performance.
* `[ssao] enable` and `[ssao] strength` — contact shading. Turning it off is the
  second-biggest performance win.
* `[water] reflect_enable` — screen-space reflections on water.
* `[upscale] texture_filter` and `[sprites] filter` — `none`, `xbr` or `scalefx`.
  `scalefx` is crisper and never blends; `none` is the sharpest, most faithful to
  the original pixels.
* `[bullettime] enable` — the slow-motion dip on big explosions. Purely a matter
  of taste.

---

## 5. Tuning lighting in-game

Press **F7** to open the lighting panel in the bottom-right corner.

| Key | Action |
| --- | --- |
| `F7` | Show / hide the panel |
| `Tab`, `KP 8` / `KP 2` | Move between sliders |
| `KP 4` / `KP 6` | Decrease / increase the selected value |
| Hold `Shift` | Fine steps |
| `KP 7` | Save `[defaultlighting]`, `[ssao]`, `[sun]` and `[thing_categories]` back to `fx3d.ini` |

Changes apply live. Nothing is written to disk until you press `KP 7`, so you can
experiment freely and just not save.

Note that saving **rewrites those four sections in place**. Keep your own notes
above a section header rather than inside it, or they will be lost.

---

## 6. Performance

In rough order of cost, turn these down first:

1. `[fx3d] AntiAliasing = 0`
2. `[ssao] enable = 0`
3. `[water] reflect_enable = 0`
4. `[sun] enable = 0` (it is already off by default)
5. `[upscale] texture_filter = none` and `[sprites] filter = none` — these cost
   memory and loading time rather than frame time, but help on weak GPUs.
6. `[fx3d] TargetFPS = 60` — capping the present rate frees the GPU.

The simulation rate never changes, so none of this affects game speed.

---

## 7. Troubleshooting

**The game window is black, or it closes at once.**
Check `error.log` in the game folder. A line about the GL context means the
driver could not give a 3.3 core context — update your graphics driver. If that
fails, `syndwarsfx.exe` (the software renderer) still works.

**Everything is the wrong brightness after tuning.**
Delete `conf/fx3d.ini`. The compiled-in defaults are identical to the shipped
file, so the game will start up looking correct and you can re-create the file
from the package.

**My edits to `fx3d.ini` do nothing.**
Make sure you are editing the `conf/fx3d.ini` inside the folder you actually run
the game from — not one in a build or source tree. If the game cannot open the
file at all it says so in `error.log` and falls back to the compiled-in defaults,
which look the same, so a wrong path is easy to miss.

**Sprite outlines look jagged even with anti-aliasing on.**
Sprites are cut out with an alpha test, so ordinary MSAA cannot smooth them. Set
`[sprites] edge_aa = 1` — it needs `[fx3d] AntiAliasing` to be 2 or higher.

**Water reflections flicker or show hard edges.**
Raise `[water] reflect_blur`. Setting `[water] reflect_debug = 1` colours the
water by why each reflection ray ended, which the comments in the ini explain.

---

## 8. Reporting problems

Include:

* your GPU and driver version,
* the `error.log` from the game folder,
* your `conf/fx3d.ini` if you changed anything,
* whether `syndwarsfx.exe` (software renderer) has the same problem.
