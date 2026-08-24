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
  light colour table (`fx3d.ini [fx3d_lights]` section)
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
- `conf/fx3d.ini` — all lighting config (shipped with the game)

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
| KP 7 | Save current values to `fx3d.ini` |

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
current values back to `fx3d.ini` so they persist between sessions.

---

### Object + Light Browser — F5

A two-mode inspector overlay. Toggle with **F5** while in-game.

**Object Browser mode** — lists all `Things` in the current level with their
Type, SubType, map position, and the light category assigned to them
(filler / building / street). Use **KP 4 / KP 6** to step through objects.
Press **KP 5** to assign/cycle the category for the highlighted thing and
**KP 7** to save all category assignments to `fx3d.ini`. PageUp /
PageDown / Home adjust the camera tilt to frame the selected object.

**Light Browser mode** — lists all `FullLight` slots, showing their ID,
Intensity, position and on/off state. Useful for cross-referencing with the
light histogram file.

Switch between modes with the mode key shown in the panel header.

---

### ThingNo Overlay

Draws the numeric Thing index above every in-game object so individual Things
can be identified for debugging. Enable in `fx3d.ini`:

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

Set in `fx3d.ini [ssao]`:

```ini
[ssao]
debug = 0   ; 0=final render, 1=world-position buffer, 2=raw AO, 3=blurred AO
```

Lets you inspect each stage of the SSAO pipeline to diagnose radius, bias or
strength issues.

---

### Sun Shadow Debug

Set in `fx3d.ini [sun]`:

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

### Phase 7 — Vehicles ✅ (2026-06-24)

Moving, rotating vehicles render as 3D geometry with metallic paint and lights.

Implemented:
- **3D bodies tracking movement**: vehicle (and turret/rotor) faces use the live
  Thing position + rotation matrix instead of the cached building placement.
  `obj_snap` in `source_sw.c` captures per-object position/matrix at floor-gate
  time (so it matches the camera snapshot); `hwr_rotate_point()` rotates points.
  - Dynamic path gated to: `TT_VEHICLE`, `TT_BUILDING/SubTT_BLD_MGUN` (turrets,
    Y>>5), `TT_BUILDING/SubTT_BLD_MOVN_ROTOR` (Y>>8). Everything else keeps the
    static cached path — fixed regressions where buildings floated and gates
    broke.
  - **Y scale gotcha**: vehicles use `PRCCOORD_TO_YCOORD` = `Y>>5` (not `>>8`).
- **Chameleon / spectraflair paint**: reflective faces (`GFlags & 0x80`) are
  diverted to a dedicated `hwr_reflect_render` pass (in `hwr_floor.c`) — a
  procedural fragment shader does a deep, view-angle hue sweep
  (green→blue→purple) + thin sweeping streak highlights, modulated by scene
  lighting so it darkens in shadow. SW `DrIT_ObFace*Refl` suppressed under FX3D.
- **Headlights + tail lights** as point lights injected into `sw_get_lights`
  (reuses the floor/face radial-lighting path — round pools on road/buildings,
  no separate pass): two white **egg-shaped** headlights (the point light gained
  a forward dir `fdx/fdz` + teardrop falloff in the floor shader — narrow/bright
  near the lamp, widening forward) and two round red tails. Reserved light slots
  so they aren't starved; culled to a view-shifted disc (camera is angled).
  A car's own lamps are excluded from its paint (`hwr_sw_vehicle_lights`) so it
  doesn't self-illuminate.
- **Cornering lean halved**: `hwr_reduce_tilt()` blends the body's up-axis
  partway back to world-up for rendering only (physics unchanged).

NOT done (deferred): flashing police lights (RM 2), per-headlight cast shadows
(RM); vehicle ground shadow darkening was attempted and reverted (the SW model
shadow is invisible over the keyed floor; see Phase 10 notes).

---

### Phase 8 — Transparency ✅ (faces/deep-radar/glass, 2026-06-25)

Verified in-game: deep-radar buildings render as flat semi-transparent
**syndicate-purple silhouettes** (palette index `deep_radar_surface_col`=216, via
a page-254 shader sentinel — texture dropped, no window holes, matching the SW
look); glass/fence (SW mode-6) faces render textured see-through; sort + depth
correct. Two hard-won fixes during bring-up: (1) the deep-radar per-object mask
must be cleared exactly **once per frame** at the top of `process_engine_unk3`
(not in `reset_drawlist`, which runs several times per frame, nor in
`sw_get_faces`, which runs several times per GL present) — otherwise the
see-through buildings flicker/disappear; (2) deep-radar faces must lose their
texture and use the flat tint, else they look like ordinary textured-transparent
buildings rather than the SW purple fill.

**Translucent sprites — deferred to a focused follow-up.** The GL translucent
billboard pass (`hwr_sprites_trans_render`), shader `uAlpha`, sort and
`[transparency]` sprite config are in place, but: (a) flagging the existing
light/glow emitters translucent made them vanish (an unresolved issue in that
pass), so emitters were reverted to opaque; (b) fire/smoke/explosions are **not**
on the billboard path at all — they come from dedicated SW arrays
(`DrIT_SFireFlame`, `DrIT_SFrmPhwoar`, `DrIT_SharpnlPoly`) and still render via
the SW overlay. Converting those to GL needs new collection paths and is its own
task.

Original implementation notes follow.



Semi-transparent rendering via a new sorted, alpha-blended GL pass. Four sources:

1. **Static glass/fence faces** — faces whose SW render mode is transparent
   (`vec_mode = face Flags`, mode **6** = `trig_render_md06` wire fence / glass)
   are diverted in `source_sw.c:sw_get_faces` into a separate transparent batch.
2. **Deep-radar see-through buildings** — captured at SW draw time:
   `draw_object` (`engindrwlstm_wrp.c`) sets a per-object bit in
   `hwr_obj_transp_mask` whenever `DrwObjF_StartBelowWindow` survives (the
   existing deep-radar test), the mask is cleared each frame in
   `reset_drawlist`, and `sw_get_faces` routes every face of a flagged object
   into the transparent batch. The SW `DrIT_ObFace*Tran` draws are now added to
   `drawitem_is_suppressed_face` so they don't paint over the 3D scene.
3. **Translucent sprites** — light/glow emitter billboards are flagged
   `HWR_BILLBOARD_TRANSLUCENT` in `sw_get_sprites` and drawn in a new blended
   billboard pass `hwr_sprites_trans_render` (alpha blend, depth-test no-write).
   The opaque sprite pass skips translucent billboards (and draws everything when
   the translucent pass is disabled, so nothing vanishes).
4. **Vehicles** — transparent vehicle faces (glass canopies) flow through the
   same face split; the divert sits after the `obj_snap`/`hwr_rotate_point`
   dynamic transform, so glass tracks the moving body.

**Pipeline:**
- New scene-source getter `get_transparent_faces` (reuses `HwrGeometryBatch` /
  `HwrVertex`). `sw_get_transparent_faces` triangle-sorts the batch back-to-front
  by centroid `face_scrd` depth for correct alpha compositing.
- New face pass `hwr_transparent_render` (in `hwr_floor.c`, reuses the floor/face
  program + texture pages + lighting) and sprite pass `hwr_sprites_trans_render`
  (in `hwr_sprite.c`). Both: `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`,
  `glDepthMask(GL_FALSE)`, depth-test `LEQUAL`. Inserted in `hwrender_glue.c`
  after `hwr_sprites_render` and before `hwr_ssao_resolve` (drawn into the same
  G-buffer/depth, get the AO composite). The floor/face and sprite fragment
  shaders gained a `uAlpha` uniform (1.0 for opaque passes).
- Tunable via `fx3d.ini [transparency]`: `enable`, `alpha`,
  `sprite_enable`, `sprite_alpha` (parsed in `hwr_lights.c`, applied through
  `HwrLightDefaults` + `hwr_transparent_config` / `hwr_sprites_trans_config`).
  Index-0 keying (window/grate holes) is preserved in the blended pass.

Builds clean (win32 MINGW32). **Pending:** in-game verification of deep-radar
see-through, glass/fence faces, glow-sprite blending, and the back-to-front sort.
Possible follow-up: collect SW fire/flame (`draw_fire_flame`) and smoke
(`draw_phwoar`) effects — currently still SW-drawn — into the translucent sprite
pass (additive blend) for full effect transparency.

---

### Phase 9 — Camera Refinements

Derive accurate camera parameters from `src/lvdraw3d.c`. Currently using
`engn_anglexz` / `overall_scale` with a placeholder isometric pitch. Phase 9
aligns the GL projection precisely with the SW engine view. Enable ini option to pitch a full 90 degrees (sprites will look weird). Enable contol of rotation and pitch with middle mouse hold.

---

### Phase 10 — Final v1 (Faithful FX3D) Polish

Bug fixing, edge-case handling, performance tuning, and final lighting
calibration pass across multiple levels. Better rain atmospherics. Full-framerate scrolling (game ticks at 16 fps, scroll at monitor refresh hz)

**Full-framerate scrolling — investigation notes (2026-06-23):**
The loop is hard-locked to 16 fps: `is_game_turn_due()` always returns true and
`wait_next_displayframe()` == `wait_next_gameturn()` (`src/game_speed.c`); sim
and display are not decoupled (the documented TODO on `game_num_fps`). Vsync is
already on (`SDL_GL_SetSwapInterval(1)` in `hwr_init.c`), so the present blocks
to monitor Hz — decoupling is the only blocker.

A **GL-only camera-interpolation** spike was tried and reverted: decouple the
loop (time-based `is_game_turn_due`), keep two camera keyframes in
`source_sw.c`, and blend them per display frame. It made *everything shake
violently* when the camera moved. Root cause: the SW overlay composited over the
3D scene (HUD, shadows, vehicle chrome/reflective faces) is **frozen** for the
whole 62 ms sim-turn, so interpolating only the GL world slides it under the
frozen overlay and snaps back each turn — a 16 Hz sawtooth. **Conclusion:
GL-only interpolation is not viable while any world-locked SW overlay is drawn
over the GL scene.**

Viable approaches (pick when tackling this):
1. Re-render *both* layers per display frame with an interpolated camera — keeps
   them in sync, but runs SW rasterisation at screen Hz (~4× load).
2. Migrate the remaining world-locked SW overlay (shadows, reflective/chrome
   faces) into the GL pipeline, leaving only the screen-space HUD in SW — then
   GL-only interpolation works. Preferred long-term.
3. Full decoupling with Thing-position interpolation (smooth moving objects too)
   — overlaps with v2.0 **RM 5** (motion tweening).

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