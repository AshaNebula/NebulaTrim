// Video preview powered by libmpv, rendered into an OpenGL texture that the UI draws.
#pragma once

#include <cstdint>
#include <string>

struct mpv_handle;
struct mpv_render_context;
struct SDL_Window;

class Player {
public:
    Player() = default;
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // The window's GL context must be current.
    bool init(SDL_Window* window, std::string* error);
    void shutdown();

    // `heavy` enables hardware decoding for this file (4K and similar).
    void load(const std::string& path, double start, bool heavy);
    void unload();
    void seek(double seconds);
    void setPaused(bool paused);
    void setSpeed(double speed);
    void frameStep(bool forward);
    void setMuted(bool muted);

    // Main thread, once per loop iteration.
    void processEvents();
    bool wantsRender() const { return renderPending_; }
    // Renders the current frame into the texture (only when needed). GL context must be current.
    void render(int width, int height);
    void reportSwap();

    unsigned texture() const { return tex_; }
    const std::string& path() const { return path_; }
    bool hasFile() const { return !path_.empty(); }
    double position() const { return pos_; }
    bool paused() const { return paused_; }
    bool eof() const { return eof_; }
    // No load/seek in flight and the reported position is trustworthy.
    bool settled() const;
    std::string takeError();

private:
    void issueSeek(double seconds);
    bool busy() const { return loading_ || pendingSeeks_ > 0 || deferredSeek_ >= 0; }

    mpv_handle* mpv_ = nullptr;
    mpv_render_context* render_ = nullptr;
    unsigned fbo_ = 0, tex_ = 0;
    int texW_ = 0, texH_ = 0;
    bool renderPending_ = false;

    std::string path_;
    double pos_ = 0;
    bool paused_ = true;
    bool eof_ = false;
    bool loading_ = false;
    int pendingSeeks_ = 0;
    double deferredSeek_ = -1;
    uint64_t restartMs_ = 0;
    int64_t expectedEntry_ = -1;
    bool currentIsExpected_ = false;
    std::string error_;
};
