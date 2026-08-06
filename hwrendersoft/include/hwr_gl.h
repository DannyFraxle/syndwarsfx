/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/** @file hwr_gl.h
 *     Minimal self-contained OpenGL 3.3 core loader.
 * @par Purpose:
 *     Declares the subset of GL types, enums and entry points the renderer
 *     uses, plus function pointers resolved at runtime via
 *     SDL_GL_GetProcAddress. This avoids a build-time dependency on system GL
 *     headers or a generated GLAD, and avoids linking -lGL directly.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef HWR_GL_H
#define HWR_GL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/

/* --- GL base types (stable across platforms for the core profile) --- */
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef void           GLvoid;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef float          GLclampf;
typedef char           GLchar;
typedef unsigned char  GLubyte_compat;
typedef ptrdiff_t      GLintptr;
typedef ptrdiff_t      GLsizeiptr;

/* --- Enums we use --- */
#define GL_FALSE                          0
#define GL_TRUE                           1
#define GL_NONE                           0
#define GL_TRIANGLES                      0x0004
#define GL_TRIANGLE_STRIP                 0x0005
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_UNSIGNED_SHORT                 0x1403
#define GL_UNSIGNED_INT                   0x1405
#define GL_FLOAT                          0x1406
#define GL_COLOR_BUFFER_BIT               0x00004000
#define GL_DEPTH_BUFFER_BIT               0x00000100
#define GL_DEPTH_TEST                     0x0B71
#define GL_MULTISAMPLE                    0x809D
#define GL_CULL_FACE                      0x0B44
#define GL_FRONT                          0x0404
#define GL_BACK                           0x0405
#define GL_CW                             0x0900
#define GL_CCW                            0x0901
#define GL_BLEND                          0x0BE2
#define GL_ONE                            0x0001
#define GL_SRC_ALPHA                      0x0302
#define GL_ONE_MINUS_SRC_ALPHA            0x0303
#define GL_LEQUAL                         0x0203
#define GL_TEXTURE_2D                     0x0DE1
#define GL_MAX_TEXTURE_SIZE               0x0D33
#define GL_TEXTURE0                       0x84C0
#define GL_TEXTURE1                       0x84C1
#define GL_TEXTURE2                       0x84C2
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_NEAREST                        0x2600
#define GL_LINEAR                         0x2601
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_TEXTURE_2D_ARRAY               0x8C1A
#define GL_TEXTURE_WRAP_R                 0x8072
#define GL_RED                            0x1903
#define GL_RGB                            0x1907
#define GL_RGB8                           0x8051
#define GL_R8                             0x8229
#define GL_RG                             0x8227
#define GL_RG8                            0x822B
#define GL_UNPACK_ALIGNMENT               0x0CF5
#define GL_ARRAY_BUFFER                   0x8892
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_STATIC_DRAW                    0x88E4
#define GL_DYNAMIC_DRAW                   0x88E8
#define GL_STREAM_DRAW                    0x88E0
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_VERSION                        0x1F02
#define GL_NO_ERROR                       0
#define GL_REPEAT                         0x2901
#define GL_RGB16F                         0x881B
#define GL_RGB32F                         0x8815
#define GL_RGBA16F                        0x881A
#define GL_RGBA32F                        0x8814
#define GL_R16F                           0x822D
#define GL_RGBA                           0x1908
#define GL_HALF_FLOAT                     0x140B
/* Framebuffer / renderbuffer objects (for the SSAO G-buffer). */
#define GL_FRAMEBUFFER                    0x8D40
#define GL_READ_FRAMEBUFFER               0x8CA8
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#define GL_RENDERBUFFER                   0x8D41
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_COLOR_ATTACHMENT1              0x8CE1
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_DEPTH_COMPONENT24              0x81A6
#define GL_DEPTH_COMPONENT                0x1902
#define GL_TEXTURE3                       0x84C3
#define GL_TEXTURE4                       0x84C4
#define GL_TEXTURE5                       0x84C5
#define GL_TEXTURE6                       0x84C6
#define GL_TEXTURE7                       0x84C7
#define GL_TEXTURE_3D                     0x806F
#define GL_POLYGON_OFFSET_FILL            0x8037

/* GL entry points use the platform's GL calling convention. On Windows that is
 * __stdcall (APIENTRY); calling through a cdecl pointer there corrupts the
 * stack. On other platforms it is the default convention. */
#if defined(_WIN32)
# define HWR_APIENTRY __stdcall
#else
# define HWR_APIENTRY
#endif

/* --- Function-pointer types --- */
typedef void   (HWR_APIENTRY *PFN_glClear)(GLbitfield);
typedef void   (HWR_APIENTRY *PFN_glClearColor)(GLclampf, GLclampf, GLclampf, GLclampf);
typedef void   (HWR_APIENTRY *PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void   (HWR_APIENTRY *PFN_glEnable)(GLenum);
typedef void   (HWR_APIENTRY *PFN_glDisable)(GLenum);
typedef void   (HWR_APIENTRY *PFN_glBlendFunc)(GLenum, GLenum);
typedef void   (HWR_APIENTRY *PFN_glDepthFunc)(GLenum);
typedef void   (HWR_APIENTRY *PFN_glDepthMask)(GLboolean);
typedef void   (HWR_APIENTRY *PFN_glCullFace)(GLenum);
typedef void   (HWR_APIENTRY *PFN_glFrontFace)(GLenum);
typedef GLenum (HWR_APIENTRY *PFN_glGetError)(void);
typedef const GLubyte_compat *(HWR_APIENTRY *PFN_glGetString)(GLenum);
typedef void   (HWR_APIENTRY *PFN_glDrawElements)(GLenum, GLsizei, GLenum, const void *);
typedef void   (HWR_APIENTRY *PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void   (HWR_APIENTRY *PFN_glPixelStorei)(GLenum, GLint);
typedef void   (HWR_APIENTRY *PFN_glGetIntegerv)(GLenum, GLint *);

typedef void   (HWR_APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void   (HWR_APIENTRY *PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void   (HWR_APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void   (HWR_APIENTRY *PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void   (HWR_APIENTRY *PFN_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);

typedef void   (HWR_APIENTRY *PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void   (HWR_APIENTRY *PFN_glDeleteVertexArrays)(GLsizei, const GLuint *);
typedef void   (HWR_APIENTRY *PFN_glBindVertexArray)(GLuint);
typedef void   (HWR_APIENTRY *PFN_glEnableVertexAttribArray)(GLuint);
typedef void   (HWR_APIENTRY *PFN_glDisableVertexAttribArray)(GLuint);
typedef void   (HWR_APIENTRY *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void   (HWR_APIENTRY *PFN_glVertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const void *);

typedef void   (HWR_APIENTRY *PFN_glGenTextures)(GLsizei, GLuint *);
typedef void   (HWR_APIENTRY *PFN_glDeleteTextures)(GLsizei, const GLuint *);
typedef void   (HWR_APIENTRY *PFN_glBindTexture)(GLenum, GLuint);
typedef void   (HWR_APIENTRY *PFN_glActiveTexture)(GLenum);
typedef void   (HWR_APIENTRY *PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void   (HWR_APIENTRY *PFN_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void   (HWR_APIENTRY *PFN_glTexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void   (HWR_APIENTRY *PFN_glTexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void   (HWR_APIENTRY *PFN_glTexParameteri)(GLenum, GLenum, GLint);

typedef GLuint (HWR_APIENTRY *PFN_glCreateShader)(GLenum);
typedef void   (HWR_APIENTRY *PFN_glDeleteShader)(GLuint);
typedef void   (HWR_APIENTRY *PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void   (HWR_APIENTRY *PFN_glCompileShader)(GLuint);
typedef void   (HWR_APIENTRY *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void   (HWR_APIENTRY *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (HWR_APIENTRY *PFN_glCreateProgram)(void);
typedef void   (HWR_APIENTRY *PFN_glDeleteProgram)(GLuint);
typedef void   (HWR_APIENTRY *PFN_glAttachShader)(GLuint, GLuint);
typedef void   (HWR_APIENTRY *PFN_glLinkProgram)(GLuint);
typedef void   (HWR_APIENTRY *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void   (HWR_APIENTRY *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void   (HWR_APIENTRY *PFN_glUseProgram)(GLuint);
typedef GLint  (HWR_APIENTRY *PFN_glGetUniformLocation)(GLuint, const GLchar *);
typedef void   (HWR_APIENTRY *PFN_glUniform1i)(GLint, GLint);
typedef void   (HWR_APIENTRY *PFN_glUniform1f)(GLint, GLfloat);
typedef void   (HWR_APIENTRY *PFN_glUniform1fv)(GLint, GLsizei, const GLfloat *);
typedef void   (HWR_APIENTRY *PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void   (HWR_APIENTRY *PFN_glUniform3f)(GLint, GLfloat, GLfloat, GLfloat);
typedef void   (HWR_APIENTRY *PFN_glUniform3fv)(GLint, GLsizei, const GLfloat *);
typedef void   (HWR_APIENTRY *PFN_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (HWR_APIENTRY *PFN_glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
typedef void   (HWR_APIENTRY *PFN_glUniform2fv)(GLint, GLsizei, const GLfloat *);

/* Framebuffer / renderbuffer objects. */
typedef void   (HWR_APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void   (HWR_APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void   (HWR_APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void   (HWR_APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void   (HWR_APIENTRY *PFN_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef GLenum (HWR_APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);
typedef void   (HWR_APIENTRY *PFN_glDrawBuffers)(GLsizei, const GLenum *);
typedef void   (HWR_APIENTRY *PFN_glDrawBuffer)(GLenum);
typedef void   (HWR_APIENTRY *PFN_glReadBuffer)(GLenum);
typedef void   (HWR_APIENTRY *PFN_glPolygonOffset)(GLfloat, GLfloat);
typedef void   (HWR_APIENTRY *PFN_glGenRenderbuffers)(GLsizei, GLuint *);
typedef void   (HWR_APIENTRY *PFN_glDeleteRenderbuffers)(GLsizei, const GLuint *);
typedef void   (HWR_APIENTRY *PFN_glBindRenderbuffer)(GLenum, GLuint);
typedef void   (HWR_APIENTRY *PFN_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (HWR_APIENTRY *PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);

/* --- The resolved entry points (defined in hwr_gl.c) --- */
#define HWR_GL_FUNC(ret, name, args) extern PFN_##name name;
#include "hwr_gl_funcs.inc"
#undef HWR_GL_FUNC

/** Resolve all entry points via SDL_GL_GetProcAddress. Returns HWR_OK (0) if
 *  every required function loaded, HWR_ERROR otherwise (and names the first
 *  missing function through hwr_last_error()). Call once, after the context is
 *  current. */
int hwr_gl_load(void);

/** Log and clear any pending GL error; returns nonzero if one was present.
 *  where is a short tag included in the log message. */
int hwr_gl_check(const char *where);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
