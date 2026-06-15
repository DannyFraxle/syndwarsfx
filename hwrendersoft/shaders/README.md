# FX3D shaders

GLSL sources for the OpenGL hardware renderer, loaded at runtime.

Populated from Phase 2 onward:

- `floor.vert` / `floor.frag` — floor tiles, depalettising fragment stage
- `face.vert` / `face.frag` — object/building faces, point lighting
- `sprite.vert` / `sprite.frag` — camera-facing billboards

All fragment shaders sample an indexed `GL_R8` atlas through a `GL_R8` palette
texture (6-bit values expanded as `(in*255+31)/63`) so coloured-lighting tints
are preserved.
