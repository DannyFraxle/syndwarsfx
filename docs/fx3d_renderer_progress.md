# Syndicate Wars FX3D — Hardware Renderer Progress

**Project:** OpenGL 3.3 core hardware renderer built alongside the existing
software path (`swrendersoft/`), housed in `hwrendersoft/` and compiled into
`libhwrender.a`. Gated behind `--enable-hwrender` at configure time and the
`--hwrender` runtime flag, so the default/CI build is byte-for-byte unaffected.

---

## Phases Complete

### Phase 1 — Scaffold (2026-06-15)

Basic OpenGL plumbing: the `hwrendersoft/` autotools subdir compiles and links
into `syndwarsfx.exe` without breaking the software-only build.

- `hwr_api.h` — public renderer API  
- `hwr_gl.h` / `hwr_gl_funcs.inc` — self-contained GL loader via
  `SDL_GL_GetProcAddress` (no GLAD, no `-lGL`), X-macro entry-point list  
- `hwr_init.c` — context creation, GL function loading  
- `hwr_draw.c` — skeleton draw/present  
- `src/hwrender_glue.c` — host-side glue (inert stubs without `HAVE_HWRENDER`)  
- `src/display.c` — `swap_wscreen()` calls `hwrender_present_frame()` first  
- `--hwrender` CLI flag wired in `main.c`  
- `lbUseOpenGLWindow` flag added to bflibrary SDL2 `sscreen.c` to request
  `SDL_WINDOW_OPENGL` at window creation

---

### Phase 2 — Software Framebuffer Through GL (2026-06-15)

The entire existing software-rendered game now displays through OpenGL. The
8-bit indexed `WScreen` buffer is uploaded as a `GL_R8` texture each frame and
depalettised in a fragment shader against a 256×1 palette texture drawn as a
fullscreen quad. The game looks and plays identically to software mode.

- `hwr_blit.c` — fullscreen blit pass, keyed (transparent index) and plain
  variants for the composite overlay used by later phases  
- Palette source is `lbPaletteColors[]` (the active SDL palette, full 8-bit)  
- `lbScreenSwapHook` set to `glue_present` so palette fades present correctly  
- Mouse coordinate scaling corrected for GL window vs mode size mismatch  
- Window focus: `SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH` + explicit
  `SDL_RaiseWindow` / `SDL_SetWindowInputFocus` at startup  
- bflibrary SDL2 backend creates a shadow 32-bpp surface in GL mode so all
  software-surface paths (palette/lock/swap) remain valid

---

### Phase 3 — Floor as 3D Geometry (2026-06-15)

Floor tiles render as real 3D textured quads, depth-tested in OpenGL, matching
the software look exactly. Buildings and columns are correctly hidden by the
depth buffer.

Key implementation details:
- Per-vertex depth = tile `scrd` value (the SW bucket-sort key), NDC mapped as
  `scrd / 16384`
- UV corner mapping derived from SW `draw_floor_tile1a`: v0→TMap4, v1→TMap3,
  v2→TMap2, v3→TMap1; triangle split (0,3,1)+(1,3,2)
- `Texture==0` (wall/column) cells inherit texture + shade from nearest valid
  8-neighbour floor tile
- `corner_alt()` uses the cell centre Alt to avoid gaps at ledge edges
- Index buffer is `uint32` (vertex count exceeds 65535)
- **Hard-won fix:** GL context had 0 depth bits because `SDL_GL_SetAttribute`
  for `SDL_GL_DEPTH_SIZE` must be called before `SDL_CreateWindow`, not after

---

### Phase 4 — Building and Object Faces (2026-06-16)

Building facades and object faces render as 3D geometry, reusing the floor
shader, texture pages, palette, and depth buffer. The scene is now fully
3D-occluded.

Key implementation details:
- World position: `worldX = MapX + pt.X`, `worldY = OffsetY + pt.Y`,
  `worldZ = MapZ + pt.Z` (MapX/Z are world units, tile×256)
- face4 uses `game_textures`; face3 uses `game_face_textures` (different
  struct layouts — using the wrong one produces magenta gaps)
- face4 triangle winding: (PN0,PN2,PN1)+(PN3,PN1,PN2), diagonal PN1–PN2
- `Texture==0` faces are flat-shaded (page=255 sentinel in shader)
- `FGFlg_Unkn20` (0x20) swaps UV corners PN2↔PN3
- SW opaque face drawing suppressed when FX3D is active via
  `engine_hwr_suppress_faces` gate in `engindrwlstx.c`
- Per-vertex `scrd` depth (face centroid depth causes z-fighting on
  angled/curved faces)

---

### Phase 5 — Point Lighting + Shadow Maps (2026-06-20)

Full dynamic lighting system. The floor and face shader evaluates up to 64
point lights per frame plus a directional sun with PCF shadow mapping. Geometric
corner AO is baked per-vertex.

**Point lights:**
- `sw_get_lights()` in `source_sw.c` walks `game_full_lights[]`, picks the
  nearest N lights to camera, converts `FullLight.Command` to RGB via the
  light colour table (`fx3d_lights.ini [fx3d_lights]` section)
- Negative-Intensity lights are "anti-lights" (fake shadows from the original
  SW engine), applied as a darkening term
- Per-category brightness (filler / building / street lamp) controlled by
  `Intensity` thresholds in `[defaultlighting]`

**Sun shadow map:**
- Depth pass rendered from the sun's viewpoint into a 2048×2048 shadow map
- Floor/face shader samples it with optional Gaussian PCF (kernel 0–12 texels)
- Azimuth, elevation, brightness, ambient, bias, haze all tunable at runtime

**Geometric AO:**
- `corner_ao()` counts building/column cells adjacent to each floor corner and
  bakes a darkening byte into the vertex — darker at the base of walls and in
  tight corners

**New source files (Phase 5):**
- `hwrendersoft/src/hwr_lights.c` — light colour table, ini parser, save/load
- `hwrendersoft/src/hwr_ssao.c` — screen-space AO G-buffer pipeline
- `hwrendersoft/src/hwr_sun.c` — directional sun + shadow map pass
- `hwrendersoft/src/hwr_debug.c` — shared bitmap debug font (digits + A–Z)
- `hwrendersoft/src/hwr_tuning.c` — real-time in-game tuning panel (F7)
- `hwrendersoft/src/hwr_thingbrowse.c` — in-game object + light browser (F5)
- `conf/fx3d_lights.ini` — all lighting config (shipped with the game)

---

## Debug Tools (Phase 5)

### Lighting Tuning Panel — F7

A real-time slider overlay displayed in the bottom-right corner of the screen.
Toggle with **F7** while in-game.

**Navigation:**
| Key | Action |
|-----|--------|
| Up / Down | Move between sliders |
| Left / Right | Adjust value (normal step) |
| Tab + Left/Right | Fine adjustment |
| KP 7 | Save current values to `fx3d_lights.ini` |

**Sliders:**

| Label | Controls |
|-------|----------|
| GAIN | Overall light intensity (0–2) |
| BLDG_B | Building/object light brightness (0–10) |
| STRT_B | Street lamp brightness (0–10) |
| FILL_B | Filler light brightness (0–2) |
| SUN_BR | Sun lit-ground brightness (0–1) |
| SUN_AM | Sun shadow (ambient) brightness (0–1) |
| SUN_AZ | Sun azimuth — compass direction, degrees (0=N, 90=E) |
| SUN_EL | Sun elevation above horizon, degrees (0–90) |
| SUN_HZ | Sun haze — how soft/scattered shadow edges are (0–1) |
| AMBIENT | Floor brightness in areas with no light or sun (0–1) |
| SUN_PC | PCF shadow softness kernel (0=hard, 12=very soft) |
| FILL_R | Filler light radius multiplier |
| BLDG_R | Building light radius multiplier |
| STRT_R | Street lamp radius multiplier |

Changes take effect immediately on the next frame. Press **KP 7** to write the
current values back to `fx3d_lights.ini` so they persist between sessions.

---

### Object + Light Browser — F5

A two-mode inspector overlay. Toggle with **F5** while in-game.

**Object Browser mode** — lists all `Things` in the current level with their
Type, SubType, map position, and the light category assigned to them
(filler / building / street). Use **KP 4 / KP 6** to step through objects.
Press **KP 5** to assign/cycle the category for the highlighted thing and
**KP 7** to save all category assignments to `fx3d_lights.ini`. PageUp /
PageDown / Home adjust the camera tilt to frame the selected object.

**Light Browser mode** — lists all `FullLight` slots, showing their ID,
Intensity, position and on/off state. Useful for cross-referencing with the
light histogram file.

Switch between modes with the mode key shown in the panel header.

---

### ThingNo Overlay

Draws the numeric Thing index above every in-game object so individual Things
can be identified for debugging. Enable in `fx3d_lights.ini`:

```ini
[defaultlighting]
thingno_debug = 1
```

Disable by setting it back to `0` (or removing the line). Reloads on next
game start.

---

### Light ID Histogram — `fx3d_light_ids.txt`

A one-time diagnostic dump written to the game directory when
`light_debug = 1` is set in `[defaultlighting]`. The file contains:

- A histogram of all `FullLight.Intensity` values in the current level,
  bucketed in groups of 25
- Suggested `[defaultlighting]` config lines for `filler_maxint` and
  `building_maxint` cutoffs based on the distribution

Useful when setting up lighting for a new level or after noticing that
categories are misclassified. Set back to `0` after use.

---

### SSAO Debug Modes

Set in `fx3d_lights.ini [ssao]`:

```ini
[ssao]
debug = 0   ; 0=final render, 1=world-position buffer, 2=raw AO, 3=blurred AO
```

Lets you inspect each stage of the SSAO pipeline to diagnose radius, bias or
strength issues.

---

### Sun Shadow Debug

Set in `fx3d_lights.ini [sun]`:

```ini
[sun]
debug = 1   ; show greyscale lit factor — 1=fully lit, 0=fully shadowed
```

Renders the scene as a greyscale lit/shadow map with no colour, useful for
calibrating bias, PCF kernel size, and polygon offset values.

---

## Phases Remaining — v1.0

### Phase 6 — Sprite Billboards

Camera-facing quad sprites for persons, vehicles, statics and effects.
Infrastructure partially implemented; integration blocked pending resolution
of a GL state issue that caused a black-screen regression.

Planned sub-tasks:
1. **Sprite collection gate** — suppress SW sprite draw for eligible items,
   collect into `HwrSpriteCollect` buffer
2. **Sprite rasteriser** — decode `TbSprite` RLE into an 8-bit indexed buffer
   without going through bflibrary render paths
3. **Sprite atlas** — lazy 2048×2048 `GL_R8` atlas with shelf packer, keyed
   by (frame, frv_pack, angle); cache up to 4096 unique sprites
4. **Billboard GL pass** — camera-facing quads, same `transform_shpoint`
   projection as floor; fragment shader: atlas lookup, alpha-test index 0,
   depalettise, full lighting (point lights + PCF shadow)
5. **Directional Ground shadow** — fake shadow stretched silloeutte under sprites
6. **Sun shadow proxy** — subtle shadow card cast by sprite on ground away from sun

---

### Phase 7 — Vehicles

Moving and rotating vehicle rendering in GL. Includes vehicle-mounted lights
(e.g. headlights and flashing police car lights). chameleon paint areas (also called colour-shift, flip paint, or spectraflair-style paint) & reflection on cars.

---

### Phase 8 — Transparency Sort

Correct rendering order for semi-transparent faces and objects (windows,
wire fences, tinted glass). Deep radar/close buildings transparency. Requires a sorted draw pass.

---

### Phase 9 — Camera Refinements

Derive accurate camera parameters from `src/lvdraw3d.c`. Currently using
`engn_anglexz` / `overall_scale` with a placeholder isometric pitch. Phase 9
aligns the GL projection precisely with the SW engine view. Enable ini option to pitch a full 90 degrees (sprites will look weird). Enable contol of rotation and pitch with middle mouse hold.

---

### Phase 10 — Final v1 (Faithful FX3D) Polish

Bug fixing, edge-case handling, performance tuning, and final lighting
calibration pass across multiple levels. Better rain atmospherics. Full-framerate scrolling (game ticks at 16 fps, scroll at monitor refresh hz)

---

### Phase 11 — Final v1 (Extended FX3D)

Day/Night cycle for sun lighting? - Optional
V4 Mods? - Optional
Fix end boss robot? - Optional
Show travel destination markers - Optional

---

## v2.0 Remastered — Future Phases

| Phase | Description |
|-------|-------------|
| RM 1 | Coloured lighting for static objects (per-sprite-type colour assignment) |
| RM 2 | Coloured lighting for vehicles (interchanging red-blue for police), explosions, fire, weapon effects |
| RM 3 | Enhanced sprites — hi-res sprites or 3D voxel objects, coloured transparent glow sprites/lens flares |
| RM 4 | High-detail vector vehicle models |
| RM 5 | Get everything working at full screen hz frame rate with motion tweening |
| RM 6 | Update controls to more modern system(drag box selection and ctrl-click selection) |
| RM 7 | HD textures for floor, building and vehicles, HD Videos |
| RM 8 | Enhance menus - more 3D like projector effects, Full HD fitting screen etc |