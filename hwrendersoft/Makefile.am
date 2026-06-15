ACLOCAL_AMFLAGS = --install -I m4

lib_LIBRARIES = libhwrender.a

libhwrender_a_headers_src = \
  include/hwr_api.h \
  include/hwr_gl.h \
  include/hwr_gl_funcs.inc \
  include/hwr_internal.h \
  include/hwr_scene_source.h \
  include/hwr_source_sw.h

libhwrender_a_headers_bld = \
  include/hwrenderconf.h

libhwrender_a_SOURCES = \
  $(libhwrender_a_headers_src) \
  src/hwr_gl.c \
  src/hwr_init.c \
  src/hwr_draw.c \
  src/hwr_blit.c \
  src/source_sw.c

libhwrender_a_CPPFLAGS = \
  -I"$(top_srcdir)/include" -I"$(builddir)/include"

libhwrenderheadersdir = $(includedir)/hwrender
libhwrenderheaders_HEADERS = \
  $(libhwrender_a_headers_src:%=$(srcdir)/%) \
  $(libhwrender_a_headers_bld:%=$(builddir)/%)

# GLSL shader sources are loaded at runtime; ship them in the dist tarball.
EXTRA_DIST = shaders
