// Tiny OpenGL function loader (only what NebulaTrim needs), resolved through SDL.
#pragma once

#include <SDL3/SDL_opengl.h>

#define NT_GL_FUNCS(X)                                                                                  \
    X(void, GenTextures, (GLsizei, GLuint*))                                                            \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                                   \
    X(void, BindTexture, (GLenum, GLuint))                                                              \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))   \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                                     \
    X(void, PixelStorei, (GLenum, GLint))                                                               \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                                 \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                           \
    X(void, Clear, (GLbitfield))                                                                        \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                        \
    X(void, GenFramebuffers, (GLsizei, GLuint*))                                                        \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                                               \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                          \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                              \
    X(GLenum, CheckFramebufferStatus, (GLenum))

namespace gl {
#define NT_GL_DECLARE(ret, name, args) extern ret(APIENTRY* name) args;
NT_GL_FUNCS(NT_GL_DECLARE)
#undef NT_GL_DECLARE

// Must be called with a current GL context. Returns false if something is missing.
bool load();
}  // namespace gl
