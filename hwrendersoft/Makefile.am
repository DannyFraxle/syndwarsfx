ACLOCAL_AMFLAGS = --install -I m4

lib_LIBRARIES = libhwrender.a

libhwrender_a_headers_src = \
  include/hwr_api.h \
  include/hwr_gl.h \
  include/hwr_gl_funcs.inc \
  include/hwr_internal.h \
  include/hwr_lights.h \
  include/hwr_scene_source.h \
  include/hwr_source_sw.h \
  include/hwr_tuning.h \
  include/hwr_thingbrowse.h

libhwrender_a_headers_bld = \
  include/hwrenderconf.h

libhwrender_a_SOURCES = \
  $(libhwrender_a_headers_src) \
  src/hwr_gl.c \
  src/hwr_init.c \
  src/hwr_draw.c \
  src/hwr_blit.c \
  src/hwr_floor.c \
  src/hwr_lights.c \
  src/hwr_ssao.c \
  src/hwr_sun.c \
   src/hwr_debug.c \
   src/hwr_tuning.c \
   src/hwr_thingbrowse.c \
   src/source_sw.c

libhwrender_a_CPPFLAGS = \
  -I"$(top_srcdir)/include" -I"$(builddir)/include"

libhwrenderheadersdir = $(includedir)/hwrender
libhwrenderheaders_HEADERS = \
  $(libhwrender_a_headers_src:%=$(srcdir)/%) \
  $(libhwrender_a_headers_bld:%=$(builddir)/%)

# GLSL shader sources are loaded at runtime; ship them in the dist tarball.
EXTRA_DIST = shaders
