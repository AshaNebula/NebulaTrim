#include "player.h"

#include "gl.h"
#include "util.h"

#include <SDL3/SDL.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <algorithm>
#include <clocale>
#include <cstring>
#include <vector>

namespace {

enum : uint64_t { kPropTime = 1, kPropPause, kPropEof };
enum : uint64_t { kReplySeek = 100 };

// After a seek/load completes, wait a little before trusting time-pos for playback decisions:
// observed property values can arrive slightly after the PLAYBACK_RESTART event.
constexpr uint64_t kSettleMs = 120;

void* getProcAddress(void*, const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }
void onWakeup(void*) { wakeMainLoop(); }

mpv_node stringNode(const char* s) {
    mpv_node n;
    n.format = MPV_FORMAT_STRING;
    n.u.string = const_cast<char*>(s);
    return n;
}

}  // namespace

Player::~Player() { shutdown(); }

bool Player::init(SDL_Window* window, std::string* error) {
    // libmpv requires the C numeric locale.
    std::setlocale(LC_NUMERIC, "C");

    mpv_ = mpv_create();
    if (!mpv_) {
        if (error) *error = "mpv_create() failed";
        return false;
    }

    static const char* const kOptions[][2] = {
        {"vo", "libmpv"},
        // Software decoding by default: for normal clips it needs far less RAM than GPU decoding
        // (which pulls in CUDA/VAAPI contexts) at a small CPU cost. Heavy files opt into
        // hardware decoding per file (see load()). Interop is disabled so nothing GPU-side is
        // initialized up front.
        {"hwdec", "no"},
        {"gpu-hwdec-interop", "no"},
        {"vd-lavc-threads", "4"},
        {"keep-open", "yes"},
        {"idle", "yes"},
        {"pause", "yes"},
        {"hr-seek", "yes"},  // frame-accurate seeking
        {"video-timing-offset", "0"},
        {"input-default-bindings", "no"},
        {"input-vo-keyboard", "no"},
        {"osc", "no"},
        {"osd-level", "0"},
        {"config", "no"},
        {"load-scripts", "no"},
        {"ytdl", "no"},
        {"terminal", "no"},
        {"sub-auto", "no"},
        {"sid", "no"},
        {"audio-display", "no"},
        {"audio-file-auto", "no"},
        {"demuxer-max-bytes", "32MiB"},
        {"demuxer-max-back-bytes", "8MiB"},
        {"background-color", "#0b0b0e"},  // mpv >= 0.38
        {"background", "#0b0b0e"},        // older mpv (fails harmlessly on new versions)
    };
    for (const auto& opt : kOptions) mpv_set_option_string(mpv_, opt[0], opt[1]);
    // Extra mpv options for troubleshooting, e.g. NEBULATRIM_MPV_OPTS="hwdec=no,vd-lavc-threads=2"
    if (const char* extra = SDL_getenv("NEBULATRIM_MPV_OPTS")) {
        std::string list = extra;
        size_t start = 0;
        while (start < list.size()) {
            size_t end = list.find(',', start);
            if (end == std::string::npos) end = list.size();
            const std::string item = list.substr(start, end - start);
            const size_t eq = item.find('=');
            if (eq != std::string::npos) mpv_set_option_string(mpv_, item.substr(0, eq).c_str(), item.substr(eq + 1).c_str());
            start = end + 1;
        }
    }

    if (mpv_initialize(mpv_) < 0) {
        if (error) *error = "mpv_initialize() failed";
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return false;
    }

    mpv_observe_property(mpv_, kPropTime, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, kPropPause, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, kPropEof, "eof-reached", MPV_FORMAT_FLAG);
    mpv_set_wakeup_callback(mpv_, onWakeup, nullptr);

    mpv_opengl_init_params glInit{};
    glInit.get_proc_address = getProcAddress;
    std::vector<mpv_render_param> params = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
    };
    // Hardware decoding interop (VAAPI etc.) needs the native display.
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (void* wl = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr))
        params.push_back({MPV_RENDER_PARAM_WL_DISPLAY, wl});
    else if (void* x11 = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr))
        params.push_back({MPV_RENDER_PARAM_X11_DISPLAY, x11});
    params.push_back({MPV_RENDER_PARAM_INVALID, nullptr});

    if (mpv_render_context_create(&render_, mpv_, params.data()) < 0) {
        if (error) *error = "Couldn't create the mpv OpenGL render context";
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        render_ = nullptr;
        return false;
    }
    mpv_render_context_set_update_callback(render_, onWakeup, nullptr);

    gl::GenTextures(1, &tex_);
    gl::BindTexture(GL_TEXTURE_2D, tex_);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    texW_ = texH_ = 16;
    gl::BindTexture(GL_TEXTURE_2D, 0);

    gl::GenFramebuffers(1, &fbo_);
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    GLenum status = gl::CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (error) *error = "The video framebuffer is incomplete";
        shutdown();
        return false;
    }
    return true;
}

void Player::shutdown() {
    if (render_) {
        mpv_render_context_free(render_);  // must happen before destroying the core
        render_ = nullptr;
    }
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
    if (fbo_) {
        gl::DeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (tex_) {
        gl::DeleteTextures(1, &tex_);
        tex_ = 0;
    }
}

void Player::load(const std::string& path, double start, bool heavy) {
    if (!mpv_) return;
    const std::string startStr = util::format("%.6f", std::max(0.0, start));

    // Named arguments work with every recent libmpv (the positional form changed in 0.38).
    // Per-file options: start position and, for heavy videos only, GPU decoding in copy-back
    // mode (other files keep the global setting, which can be overridden for troubleshooting).
    char* optKeys[] = {const_cast<char*>("start"), const_cast<char*>("hwdec")};
    mpv_node optValues[] = {stringNode(startStr.c_str()), stringNode("auto-copy-safe")};
    mpv_node_list optList{heavy ? 2 : 1, optValues, optKeys};
    mpv_node options;
    options.format = MPV_FORMAT_NODE_MAP;
    options.u.list = &optList;

    char* keys[] = {const_cast<char*>("name"), const_cast<char*>("url"), const_cast<char*>("flags"),
                    const_cast<char*>("options")};
    mpv_node values[] = {stringNode("loadfile"), stringNode(path.c_str()), stringNode("replace"), options};
    mpv_node_list list{4, values, keys};
    mpv_node cmd;
    cmd.format = MPV_FORMAT_NODE_MAP;
    cmd.u.list = &list;

    mpv_node result;
    std::memset(&result, 0, sizeof(result));
    expectedEntry_ = -1;
    if (mpv_command_node(mpv_, &cmd, &result) < 0) {
        error_ = "Couldn't open the video";
        return;
    }
    if (result.format == MPV_FORMAT_NODE_MAP && result.u.list) {
        for (int i = 0; i < result.u.list->num; ++i) {
            if (std::strcmp(result.u.list->keys[i], "playlist_entry_id") == 0 &&
                result.u.list->values[i].format == MPV_FORMAT_INT64)
                expectedEntry_ = result.u.list->values[i].u.int64;
        }
    }
    mpv_free_node_contents(&result);

    path_ = path;
    pos_ = start;
    eof_ = false;
    loading_ = true;
    pendingSeeks_ = 0;
    deferredSeek_ = -1;
    currentIsExpected_ = false;
}

void Player::unload() {
    if (!mpv_) return;
    const char* cmd[] = {"stop", nullptr};
    mpv_command_async(mpv_, 0, cmd);
    path_.clear();
    pos_ = 0;
    eof_ = false;
    loading_ = false;
    pendingSeeks_ = 0;
    deferredSeek_ = -1;
}

void Player::issueSeek(double seconds) {
    const std::string t = util::format("%.6f", std::max(0.0, seconds));
    const char* cmd[] = {"seek", t.c_str(), "absolute+exact", nullptr};
    if (mpv_command_async(mpv_, kReplySeek, cmd) >= 0) ++pendingSeeks_;
}

void Player::seek(double seconds) {
    if (!mpv_ || path_.empty()) return;
    pos_ = std::max(0.0, seconds);
    eof_ = false;
    if (loading_)
        deferredSeek_ = pos_;  // mpv rejects seeks until the file is loaded
    else
        issueSeek(pos_);
}

void Player::setPaused(bool paused) {
    if (!mpv_) return;
    int flag = paused ? 1 : 0;
    mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &flag);
    paused_ = paused;
}

void Player::setSpeed(double speed) {
    if (!mpv_) return;
    double s = std::clamp(speed, 0.01, 100.0);
    mpv_set_property_async(mpv_, 0, "speed", MPV_FORMAT_DOUBLE, &s);
}

void Player::setMuted(bool muted) {
    if (!mpv_) return;
    int flag = muted ? 1 : 0;
    mpv_set_property_async(mpv_, 0, "mute", MPV_FORMAT_FLAG, &flag);
}

void Player::frameStep(bool forward) {
    if (!mpv_ || path_.empty() || busy()) return;
    const char* cmd[] = {forward ? "frame-step" : "frame-back-step", nullptr};
    mpv_command_async(mpv_, 0, cmd);
    paused_ = true;
}

bool Player::settled() const {
    return mpv_ && !path_.empty() && !busy() && SDL_GetTicks() - restartMs_ > kSettleMs;
}

std::string Player::takeError() {
    std::string e;
    e.swap(error_);
    return e;
}

void Player::processEvents() {
    if (!mpv_) return;
    for (;;) {
        mpv_event* ev = mpv_wait_event(mpv_, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        switch (ev->event_id) {
        case MPV_EVENT_START_FILE: {
            auto* sf = static_cast<mpv_event_start_file*>(ev->data);
            currentIsExpected_ = expectedEntry_ < 0 || (sf && sf->playlist_entry_id == expectedEntry_);
            break;
        }
        case MPV_EVENT_FILE_LOADED:
            if (currentIsExpected_ && deferredSeek_ >= 0) {
                loading_ = false;
                issueSeek(deferredSeek_);
                deferredSeek_ = -1;
            }
            break;
        case MPV_EVENT_PLAYBACK_RESTART:
            if (currentIsExpected_) {
                loading_ = false;
                pendingSeeks_ = 0;
                restartMs_ = SDL_GetTicks();
            }
            break;
        case MPV_EVENT_END_FILE: {
            auto* ef = static_cast<mpv_event_end_file*>(ev->data);
            if (ef && ef->reason == MPV_END_FILE_REASON_ERROR &&
                (expectedEntry_ < 0 || ef->playlist_entry_id == expectedEntry_)) {
                error_ = mpv_error_string(ef->error);
                loading_ = false;
                pendingSeeks_ = 0;
                deferredSeek_ = -1;
            }
            break;
        }
        case MPV_EVENT_COMMAND_REPLY:
            if (ev->reply_userdata == kReplySeek && ev->error < 0 && pendingSeeks_ > 0) --pendingSeeks_;
            break;
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto* prop = static_cast<mpv_event_property*>(ev->data);
            if (ev->reply_userdata == kPropTime && prop->format == MPV_FORMAT_DOUBLE) {
                if (!busy()) pos_ = *static_cast<double*>(prop->data);
            } else if (ev->reply_userdata == kPropPause && prop->format == MPV_FORMAT_FLAG) {
                paused_ = *static_cast<int*>(prop->data) != 0;
            } else if (ev->reply_userdata == kPropEof && prop->format == MPV_FORMAT_FLAG) {
                if (!busy()) eof_ = *static_cast<int*>(prop->data) != 0;
            }
            break;
        }
        default:
            break;
        }
    }
    if (render_ && (mpv_render_context_update(render_) & MPV_RENDER_UPDATE_FRAME)) renderPending_ = true;
}

void Player::render(int width, int height) {
    if (!render_ || width <= 0 || height <= 0) return;
    const bool resized = width != texW_ || height != texH_;
    if (!renderPending_ && !resized) return;
    if (resized) {
        gl::BindTexture(GL_TEXTURE_2D, tex_);
        gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl::BindTexture(GL_TEXTURE_2D, 0);
        texW_ = width;
        texH_ = height;
    }
    mpv_opengl_fbo fbo{(int)fbo_, width, height, GL_RGBA8};
    int flipY = 0;
    int block = 0;  // never stall the UI thread waiting for the frame's display time
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(render_, params);
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    renderPending_ = false;
}

void Player::reportSwap() {
    if (render_) mpv_render_context_report_swap(render_);
}
