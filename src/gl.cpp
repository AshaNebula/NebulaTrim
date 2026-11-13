#include "gl.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_video.h>

namespace gl {
#define NT_GL_DEFINE(ret, name, args) ret(APIENTRY* name) args = nullptr;
NT_GL_FUNCS(NT_GL_DEFINE)
#undef NT_GL_DEFINE

bool load() {
    bool ok = true;
#define NT_GL_LOAD(ret, name, args)                                                       \
    name = reinterpret_cast<ret(APIENTRY*) args>(SDL_GL_GetProcAddress("gl" #name));     \
    if (!name) {                                                                          \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenGL: falta gl%s", #name);          \
        ok = false;                                                                       \
    }
    NT_GL_FUNCS(NT_GL_LOAD)
#undef NT_GL_LOAD
    return ok;
}
}  // namespace gl
