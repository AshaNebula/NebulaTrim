// NebulaTrim - tiny video trimmer/joiner.
// SDL3 (window + input) · Dear ImGui (UI) · libmpv (preview) · FFmpeg CLI (probe + export)

#include "app.h"
#include "gl.h"
#include "media.h"
#include "player.h"
#include "ui.h"
#include "util.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#ifndef NEBULATRIM_VERSION
#define NEBULATRIM_VERSION "0.1.0"
#endif

namespace {

Uint32 g_wakeEvent = 0;
std::atomic<bool> g_wakePending{false};

// UI scale in window coordinates: 1.0 on Wayland/macOS (the compositor scales), the
// monitor scale on Windows/X11 where window coordinates are physical pixels.
float contentScale(SDL_Window* window) {
    float display = SDL_GetWindowDisplayScale(window);
    float density = SDL_GetWindowPixelDensity(window);
    if (!(display > 0)) display = 1;
    if (!(density > 0)) density = 1;
    const float s = display / density;
    return (s >= 0.5f && s <= 4.0f) ? s : 1.0f;
}

void fatal(const std::string& msg, SDL_Window* window = nullptr) {
    std::fprintf(stderr, "NebulaTrim: %s\n", msg.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "NebulaTrim", msg.c_str(), window);
}

// Debug helper (NEBULATRIM_SNAPSHOT=file.ppm): saves the first fully loaded frame and exits.
bool writePPM(const char* path, int w, int h, const std::vector<unsigned char>& rgb) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y) std::fwrite(rgb.data() + (size_t)y * (size_t)w * 3, 1, (size_t)w * 3, f);
    std::fclose(f);
    return true;
}

}  // namespace

void wakeMainLoop() {
    if (!g_wakeEvent || g_wakePending.exchange(true)) return;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = g_wakeEvent;
    SDL_PushEvent(&ev);
}

int main(int argc, char** argv) {
#if defined(__GLIBC__)
    // Fewer malloc arenas: less fragmentation/RSS with mpv's many decoder threads.
    mallopt(M_ARENA_MAX, 2);
#endif
    SDL_SetAppMetadata("NebulaTrim", NEBULATRIM_VERSION, "nebulatrim");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fatal(std::string("No se pudo iniciar SDL: ") + SDL_GetError());
        return 1;
    }
    g_wakeEvent = SDL_RegisterEvents(1);
    const char* snapshotPath = SDL_getenv("NEBULATRIM_SNAPSHOT");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

    const SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    SDL_Window* window = SDL_CreateWindow("NebulaTrim", 1280, 800, flags);
    if (!window) {
        fatal(std::string("No se pudo crear la ventana: ") + SDL_GetError());
        SDL_Quit();
        return 1;
    }
    float scale = contentScale(window);
    if (scale > 1.01f) {
        int w = (int)(1280 * scale), h = (int)(800 * scale);
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable)) {
            w = std::min(w, (int)(usable.w * 0.92f));
            h = std::min(h, (int)(usable.h * 0.92f));
        }
        SDL_SetWindowSize(window, w, h);
    }
    SDL_SetWindowMinimumSize(window, (int)(820 * scale), (int)(620 * scale));
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        fatal(std::string("No se pudo crear el contexto OpenGL 3.2: ") + SDL_GetError(), window);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, glContext);
    SDL_GL_SetSwapInterval(snapshotPath ? 0 : 1);
    if (!gl::load()) {
        fatal("Tu tarjeta gráfica no soporta las funciones OpenGL necesarias.", window);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing to persist
    io.LogFilename = nullptr;
    ui::loadFonts();
    ui::applyTheme(scale);
    ImGui_ImplSDL3_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version 150");

    media::initTools();
    Player player;
    std::string error;
    if (!player.init(window, &error)) {
        fatal("No se pudo iniciar el reproductor (libmpv):\n" + error, window);
        return 1;
    }

    {
        App app(window, player);
        std::vector<std::string> files;
        for (int i = 1; i < argc; ++i) files.emplace_back(argv[i]);
        app.addFiles(files);
        if (!snapshotPath)
            SDL_ShowWindow(window);
        else
            player.setMuted(true);

        const uint64_t startMs = SDL_GetTicks();
        uint64_t snapshotAt = 0;
        bool running = true;
        int framesToDraw = 3;
        std::vector<std::string> dropped;

        auto handle = [&](const SDL_Event& ev) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                if (app.requestQuit()) running = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (ev.window.windowID == SDL_GetWindowID(window) && app.requestQuit()) running = false;
                break;
            case SDL_EVENT_DROP_FILE:
                if (ev.drop.data) dropped.emplace_back(ev.drop.data);
                break;
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
                const float s = contentScale(window);
                if (std::fabs(s - scale) > 0.01f) {
                    scale = s;
                    ui::applyTheme(scale);
                }
                break;
            }
            default:
                if (ev.type == g_wakeEvent) g_wakePending = false;
                break;
            }
        };

        while (running) {
            SDL_Event ev;
            bool hadEvents = false;
            // Sleep until something happens: no CPU use while idle.
            if (framesToDraw <= 0 && !player.wantsRender()) {
                int timeout = app.waitTimeoutMs();
                if (snapshotPath && (timeout < 0 || timeout > 100)) timeout = 100;  // poll the snapshot timer
                if (SDL_WaitEventTimeout(&ev, timeout)) {
                    handle(ev);
                    hadEvents = true;
                } else if (timeout >= 0) {
                    framesToDraw = 1;
                }
            }
            while (SDL_PollEvent(&ev)) {
                handle(ev);
                hadEvents = true;
            }
            if (!dropped.empty()) {
                app.addFiles(dropped);
                dropped.clear();
            }
            if (hadEvents) framesToDraw = std::max(framesToDraw, 2);  // ImGui needs a couple of frames to settle

            app.update();
            if (player.wantsRender()) framesToDraw = std::max(framesToDraw, 1);
            if (app.quitNow()) running = false;
            if (!running) break;
            if (framesToDraw <= 0) continue;
            --framesToDraw;

            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                int pw, ph;
                app.previewSize(pw, ph);
                if (player.wantsRender()) player.render(std::max(pw, 16), std::max(ph, 16));  // keep mpv flowing
                continue;
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            app.draw();
            ImGui::Render();

            int pw, ph;
            app.previewSize(pw, ph);
            if (pw > 0 && ph > 0) player.render(pw, ph);

            int fbw = 0, fbh = 0;
            SDL_GetWindowSizeInPixels(window, &fbw, &fbh);
            gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
            gl::Viewport(0, 0, fbw, fbh);
            gl::ClearColor(ui::color::bg.x, ui::color::bg.y, ui::color::bg.z, 1.0f);
            gl::Clear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            if (snapshotPath) {
                const uint64_t now = SDL_GetTicks();
                if (!snapshotAt && now - startMs > 800 && !app.busy() && (player.settled() || !player.hasFile())) {
                    // Optional test setup: TRIM="in,out,speed", SEEK=seconds, PLAY=1|all, EXPORT=1
                    if (const char* trim = SDL_getenv("NEBULATRIM_SNAPSHOT_TRIM")) {
                        double in = 0, out = 0, speed = 1;
                        if (std::sscanf(trim, "%lf,%lf,%lf", &in, &out, &speed) == 3) app.debugTrim(in, out, (float)speed);
                    }
                    if (const char* seek = SDL_getenv("NEBULATRIM_SNAPSHOT_SEEK")) app.debugSeek(std::atof(seek));
                    if (const char* play = SDL_getenv("NEBULATRIM_SNAPSHOT_PLAY")) app.debugPlay(std::string(play) == "all");
                    if (SDL_getenv("NEBULATRIM_SNAPSHOT_EXPORT")) app.openExportDialog();
                    const char* delay = SDL_getenv("NEBULATRIM_SNAPSHOT_DELAY_MS");
                    snapshotAt = now + (delay ? (uint64_t)std::atoll(delay) : 1200);
                }
                if (snapshotAt && now >= snapshotAt && !app.busy()) {
                    if (SDL_getenv("NEBULATRIM_DEBUG")) {  // report memory use (Linux)
                        if (FILE* st = std::fopen("/proc/self/status", "r")) {
                            char line[256];
                            while (std::fgets(line, sizeof(line), st))
                                if (!std::strncmp(line, "VmRSS", 5) || !std::strncmp(line, "VmHWM", 5)) std::fputs(line, stdout);
                            std::fclose(st);
                        }
                    }
                    std::vector<unsigned char> rgb((size_t)fbw * (size_t)fbh * 3);
                    gl::PixelStorei(GL_PACK_ALIGNMENT, 1);
                    gl::ReadPixels(0, 0, fbw, fbh, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
                    std::printf("snapshot %s: %s (%dx%d)\n", snapshotPath, writePPM(snapshotPath, fbw, fbh, rgb) ? "ok" : "error", fbw, fbh);
                    running = false;
                }
                if (now - startMs > 60000) running = false;  // safety net
            }

            SDL_GL_SwapWindow(window);
            player.reportSwap();
        }
    }

    player.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
