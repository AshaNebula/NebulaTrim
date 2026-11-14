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
#include <memory>
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

// Development aid (NEBULATRIM_SNAPSHOT=file.ppm): runs without showing the window, applies an
// optional scripted setup, replays mouse gestures through real SDL events, then saves the
// framebuffer and exits. Variables: NEBULATRIM_SNAPSHOT_{TRIM="in,out,speed", SPLIT=seconds,
// SEEK=seconds, PLAY=1|all|here, EXPORT=1, DRAG="point:dx[:shift];...", DELAY_MS=ms}.
// Gesture points: cut, clip-in, clip-out, video@<0..1>; "key:<space|a-z>[:shift]" presses a key.
class SnapshotRunner {
public:
    SnapshotRunner(const char* path, SDL_Window* window) : path_(path), window_(window), start_(SDL_GetTicks()) {
        if (const char* drag = SDL_getenv("NEBULATRIM_SNAPSHOT_DRAG")) parseGestures(drag);
    }

    // Called after each rendered frame (before the swap). Returns false when it is time to quit.
    bool afterFrame(App& app, Player& player, int fbw, int fbh) {
        const uint64_t now = SDL_GetTicks();
        if (now - start_ > 60000) return false;  // safety net
        if (!setupDone_) {
            if (now - start_ < 800 || app.busy() || !(player.settled() || !player.hasFile())) return true;
            setup(app);
            setupDone_ = true;
            gesturesAt_ = now + 300;  // let windows opened by the setup appear and settle first
            return true;
        }
        if (gesture_ < gestures_.size()) {
            if (now < gesturesAt_) return true;
            stepGesture(app);
            if (gesture_ == gestures_.size()) {
                const char* delay = SDL_getenv("NEBULATRIM_SNAPSHOT_DELAY_MS");
                captureAt_ = now + (delay ? (uint64_t)std::atoll(delay) : 1200);
            }
            return true;
        }
        if (now < captureAt_ || app.busy()) return true;

        if (SDL_getenv("NEBULATRIM_DEBUG")) {
            app.debugDump();
            if (FILE* st = std::fopen("/proc/self/status", "r")) {  // memory use (Linux)
                char line[256];
                while (std::fgets(line, sizeof(line), st))
                    if (!std::strncmp(line, "VmRSS", 5) || !std::strncmp(line, "VmHWM", 5)) std::fputs(line, stdout);
                std::fclose(st);
            }
        }
        std::vector<unsigned char> rgb((size_t)fbw * (size_t)fbh * 3);
        gl::PixelStorei(GL_PACK_ALIGNMENT, 1);
        gl::ReadPixels(0, 0, fbw, fbh, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        std::printf("snapshot %s: %s (%dx%d)\n", path_, writePPM(path_, fbw, fbh, rgb) ? "ok" : "error", fbw, fbh);
        return false;
    }

private:
    struct Gesture {
        std::string point;  // or "key"
        std::string key;    // key name for "key" gestures
        float dx = 0;
        bool shift = false;
    };

    void parseGestures(const std::string& spec) {
        size_t pos = 0;
        while (pos < spec.size()) {
            size_t end = spec.find(';', pos);
            if (end == std::string::npos) end = spec.size();
            const std::string item = spec.substr(pos, end - pos);
            pos = end + 1;
            const size_t c1 = item.find(':');
            if (c1 == std::string::npos) continue;
            Gesture g;
            g.point = item.substr(0, c1);
            g.dx = (float)std::atof(item.c_str() + c1 + 1);
            if (g.point == "key") g.key = item.substr(c1 + 1, item.find(':', c1 + 1) - (c1 + 1));
            g.shift = item.find(":shift") != std::string::npos;
            gestures_.push_back(g);
        }
        captureAt_ = 0;
    }

    void setup(App& app) {
        if (const char* trim = SDL_getenv("NEBULATRIM_SNAPSHOT_TRIM")) {
            double in = 0, out = 0, speed = 1;
            if (std::sscanf(trim, "%lf,%lf,%lf", &in, &out, &speed) == 3) app.debugTrim(in, out, (float)speed);
        }
        if (const char* split = SDL_getenv("NEBULATRIM_SNAPSHOT_SPLIT")) app.debugSplit(std::atof(split));
        if (const char* seek = SDL_getenv("NEBULATRIM_SNAPSHOT_SEEK")) app.debugSeek(std::atof(seek));
        if (const char* play = SDL_getenv("NEBULATRIM_SNAPSHOT_PLAY")) {
            const std::string mode = play;
            app.debugPlay(mode == "all" ? 1 : mode == "here" ? 2 : 0);
        }
        if (SDL_getenv("NEBULATRIM_SNAPSHOT_EXPORT")) app.openExportDialog();
        if (gestures_.empty()) {
            const char* delay = SDL_getenv("NEBULATRIM_SNAPSHOT_DELAY_MS");
            captureAt_ = SDL_GetTicks() + (delay ? (uint64_t)std::atoll(delay) : 1200);
        }
    }

    bool resolve(App& app, const std::string& point, float& x, float& y) {
        if (point.rfind("video@", 0) == 0) {
            float x0, x1, y0;
            if (!app.debugPoint("video-start", x0, y0) || !app.debugPoint("video-end", x1, y)) return false;
            x = x0 + (x1 - x0) * (float)std::atof(point.c_str() + 6);
            return true;
        }
        return app.debugPoint(point, x, y);
    }

    // One SDL event per frame: move, (shift), press, 10 moves, release, (shift up).
    void stepGesture(App& app) {
        const Gesture& g = gestures_[gesture_];
        if (g.point == "key") {  // (shift) key down, then key up (shift up)
            if (step_ == 0) {
                if (g.shift) pushShift(true);
                pushKey(g.key, true, g.shift);
                step_ = 1;
            } else {
                pushKey(g.key, false, g.shift);
                if (g.shift) pushShift(false);
                step_ = 0;
                ++gesture_;
            }
            return;
        }
        const SDL_WindowID id = SDL_GetWindowID(window_);
        SDL_Event e;
        SDL_zero(e);
        if (step_ == 0) {
            if (!resolve(app, g.point, x_, y_)) {
                std::printf("gesture: point '%s' not found\n", g.point.c_str());
                ++gesture_;
                return;
            }
        }
        const int moves = 10;
        if (step_ == 0 || (step_ >= 2 && step_ < 2 + moves)) {
            const float t = step_ == 0 ? 0.0f : (float)(step_ - 1) / moves;
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.windowID = id;
            e.motion.x = x_ + g.dx * t;
            e.motion.y = y_;
        } else if (step_ == 1 || step_ == 2 + moves) {
            const bool down = step_ == 1;
            if (down && g.shift) pushShift(true);
            e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = id;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = down;
            e.button.clicks = 1;
            e.button.x = x_ + (down ? 0.0f : g.dx);
            e.button.y = y_;
        }
        SDL_PushEvent(&e);
        if (++step_ > 2 + moves) {
            if (g.shift) pushShift(false);
            step_ = 0;
            ++gesture_;
        }
    }

    void pushKey(const std::string& name, bool down, bool shift) {
        SDL_Event e;
        SDL_zero(e);
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.windowID = SDL_GetWindowID(window_);
        if (name == "space") {
            e.key.key = SDLK_SPACE;
            e.key.scancode = SDL_SCANCODE_SPACE;
        } else if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') {
            e.key.key = (SDL_Keycode)name[0];  // SDL keycodes for letters are their lowercase ASCII
            e.key.scancode = (SDL_Scancode)(SDL_SCANCODE_A + (name[0] - 'a'));
        } else {
            std::printf("gesture: unknown key '%s'\n", name.c_str());
            return;
        }
        e.key.mod = shift ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE;  // the backend reads modifiers from each key event
        e.key.down = down;
        SDL_PushEvent(&e);
    }

    void pushShift(bool down) {
        SDL_Event e;
        SDL_zero(e);
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.windowID = SDL_GetWindowID(window_);
        e.key.key = SDLK_LSHIFT;
        e.key.scancode = SDL_SCANCODE_LSHIFT;
        e.key.mod = down ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE;
        e.key.down = down;
        SDL_PushEvent(&e);
    }

    const char* path_;
    SDL_Window* window_;
    uint64_t start_;
    bool setupDone_ = false;
    std::vector<Gesture> gestures_;
    size_t gesture_ = 0;
    int step_ = 0;
    float x_ = 0, y_ = 0;
    uint64_t captureAt_ = 0;
    uint64_t gesturesAt_ = 0;
};

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
        fatal(std::string("Couldn't initialize SDL: ") + SDL_GetError());
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
        fatal(std::string("Couldn't create the window: ") + SDL_GetError());
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
        fatal(std::string("Couldn't create an OpenGL 3.2 context: ") + SDL_GetError(), window);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, glContext);
    SDL_GL_SetSwapInterval(snapshotPath ? 0 : 1);
    if (!gl::load()) {
        fatal("Your graphics driver doesn't support the required OpenGL functions.", window);
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
        fatal("Couldn't start the video player (libmpv):\n" + error, window);
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

        std::unique_ptr<SnapshotRunner> snapshot;
        if (snapshotPath) snapshot = std::make_unique<SnapshotRunner>(snapshotPath, window);
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

            if (snapshot && !snapshot->afterFrame(app, player, fbw, fbh)) running = false;

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
