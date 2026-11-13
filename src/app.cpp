#define IMGUI_DEFINE_MATH_OPERATORS
#include "app.h"

#include "gl.h"
#include "player.h"
#include "ui.h"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace {

constexpr double kMinClip = 0.10;  // shortest clip allowed (seconds)
constexpr int kThumbW = 256, kThumbH = 144;
constexpr size_t kMaxUndo = 100;

std::mutex g_appMutex;
App* g_app = nullptr;

// NEBULATRIM_DEBUG=1 prints playback decisions to stderr.
const bool kDebug = std::getenv("NEBULATRIM_DEBUG") != nullptr;

std::string thumbKey(const Clip& c) { return c.path + "|" + std::to_string((long long)std::llround(c.in * 10.0)); }

int evenSize(double v) { return std::max(2, (int)std::lround(v / 2.0) * 2); }

void rightAlign(float width) {
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
}

}  // namespace

struct App::Texture {
    unsigned id = 0;
    ~Texture() {
        if (id) gl::DeleteTextures(1, &id);
    }
};

// ---------------------------------------------------------------------------------------------
// Lifetime

App::App(SDL_Window* window, Player& player) : window_(window), player_(player) {
    {
        std::lock_guard<std::mutex> lock(g_appMutex);
        g_app = this;
    }
    worker_.submit([this] {
        std::string err;
        std::string enc = media::detectVideoEncoder(&err);
        mainQueue_.post([this, enc, err] {
            encoder_ = enc;
            toolsError_ = err;
            toolsChecked_ = true;
        });
    });
}

App::~App() {
    std::lock_guard<std::mutex> lock(g_appMutex);
    g_app = nullptr;
}

void App::postFromAnyThread(std::function<void(App&)> fn) {
    std::lock_guard<std::mutex> lock(g_appMutex);
    if (!g_app) return;
    App* app = g_app;
    app->mainQueue_.post([app, fn = std::move(fn)] { fn(*app); });
}

// ---------------------------------------------------------------------------------------------
// Queries

int App::indexOf(uint64_t id) const {
    if (id == 0) return -1;
    for (size_t i = 0; i < clips_.size(); ++i)
        if (clips_[i].id == id) return (int)i;
    return -1;
}

Clip* App::selectedClip() {
    const int i = selectedIndex();
    return i >= 0 ? &clips_[(size_t)i] : nullptr;
}

const MediaInfo* App::info(const std::string& path) const {
    auto it = media_.find(path);
    return it == media_.end() ? nullptr : &it->second;
}

double App::frameDuration(const Clip& c) const {
    const MediaInfo* mi = info(c.path);
    return 1.0 / (mi ? mi->fps() : 30.0);
}

double App::totalDuration() const {
    double total = 0;
    for (const Clip& c : clips_) total += (c.out - c.in) / c.speed;
    return total;
}

bool App::busy() const { return !loading_.empty() || pendingJobs_ > 0 || !toolsChecked_; }

int App::waitTimeoutMs() const {
    int timeout = -1;
    auto atMost = [&timeout](int v) {
        if (timeout < 0 || v < timeout) timeout = v;
    };
    if (!toasts_.empty()) atMost(250);
    if (export_.state() == ExportJob::State::Running) atMost(500);
    if (playing_) atMost(100);
    if (ImGui::GetIO().WantTextInput) atMost(500);
    if (ImGui::IsAnyItemHovered()) atMost(200);  // tooltips appear after a short delay
    return timeout;
}

// ---------------------------------------------------------------------------------------------
// Adding media

void App::addFiles(const std::vector<std::string>& paths) {
    for (const std::string& raw : paths) {
        if (raw.empty()) continue;
        std::string path = raw;
        std::error_code ec;
        auto abs = std::filesystem::absolute(std::filesystem::u8path(raw), ec);
        if (!ec) path = abs.lexically_normal().u8string();
        if (!util::pathExists(path)) {
            toast("No se encontró: " + util::fileName(path), true);
            continue;
        }
        lastDir_ = util::dirName(path);
        if (const MediaInfo* mi = info(path)) {
            addClip(path, *mi);
            continue;
        }
        loading_.push_back(path);
        worker_.submit([this, path] {
            MediaInfo mi = media::probe(path);
            mainQueue_.post([this, path, mi] { onProbed(path, mi); });
        });
    }
}

void App::onProbed(const std::string& path, const MediaInfo& mi) {
    auto it = std::find(loading_.begin(), loading_.end(), path);
    if (it != loading_.end()) loading_.erase(it);
    if (!mi.ok) {
        toast(util::fileName(path) + ": " + mi.error, true);
        return;
    }
    media_[path] = mi;
    addClip(path, mi);
}

void App::addClip(const std::string& path, const MediaInfo& mi) {
    pushUndo();
    Clip c;
    c.id = nextId_++;
    c.path = path;
    c.in = 0;
    c.out = mi.duration;
    clips_.push_back(c);
    if (selectedIndex() < 0) select((int)clips_.size() - 1, 0.0);
}

// ---------------------------------------------------------------------------------------------
// Playback

void App::select(int index, double position, bool scrollIntoView) {
    if (index < 0 || index >= (int)clips_.size()) {
        selectedId_ = 0;
        playing_ = false;
        playAll_ = false;
        player_.setPaused(true);
        player_.unload();
        return;
    }
    const Clip& c = clips_[(size_t)index];
    selectedId_ = c.id;
    scrollToSelected_ = scrollIntoView;
    if (player_.path() != c.path) {
        // Above ~1440p60 software decoding gets expensive: let the GPU decode those files.
        const MediaInfo* mi = info(c.path);
        const bool heavy = mi && (double)mi->width * mi->height * mi->fps() > 2.0e8;
        player_.load(c.path, position, heavy);
    } else
        player_.seek(position);
    player_.setSpeed(c.speed);
}

void App::togglePlay() {
    Clip* c = selectedClip();
    if (!c || !player_.hasFile()) return;
    if (playing_) {
        pause();
        return;
    }
    const double fd = frameDuration(*c);
    const double pos = player_.position();
    if (player_.eof() || pos < c->in - 1e-3 || pos >= c->out - 1.5 * fd) player_.seek(c->in);
    player_.setSpeed(c->speed);
    player_.setPaused(false);
    playing_ = true;
}

void App::pause() {
    playing_ = false;
    playAll_ = false;
    player_.setPaused(true);
}

void App::togglePlayAll() {
    if (playAll_) {
        pause();
        return;
    }
    if (clips_.empty()) return;
    pause();
    select(0, clips_[0].in);
    player_.setPaused(false);
    playing_ = true;
    playAll_ = true;
}

void App::seekRelative(double delta) {
    Clip* c = selectedClip();
    const MediaInfo* mi = c ? info(c->path) : nullptr;
    if (!mi) return;
    player_.seek(std::clamp(player_.position() + delta, 0.0, mi->duration));
}

void App::stepFrame(bool forward) {
    if (!selectedClip()) return;
    pause();
    player_.frameStep(forward);
}

void App::debugSeek(double seconds) {
    if (selectedClip()) player_.seek(seconds);
}

void App::debugTrim(double in, double out, float speed) {
    Clip* c = selectedClip();
    const MediaInfo* mi = c ? info(c->path) : nullptr;
    if (!mi) return;
    c->in = std::clamp(in, 0.0, mi->duration - kMinClip);
    c->out = std::clamp(out, c->in + kMinClip, mi->duration);
    c->speed = std::clamp(speed, 0.1f, 10.0f);
    select(selectedIndex(), c->in);
}

void App::debugPlay(bool all) {
    if (all)
        togglePlayAll();
    else
        togglePlay();
}

void App::update() {
    mainQueue_.run();
    player_.processEvents();
    if (std::string err = player_.takeError(); !err.empty()) {
        toast("No se pudo reproducir: " + err, true);
        pause();
    }

    const uint64_t now = SDL_GetTicks();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [now](const Toast& t) { return t.until <= now; }),
                  toasts_.end());

    Clip* c = selectedClip();
    if (!c) {
        playing_ = playAll_ = false;
        return;
    }
    // Keep playback inside the trimmed range; in "play all" mode continue with the next clip.
    if (playing_ && player_.settled() && player_.path() == c->path) {
        const double fd = frameDuration(*c);
        const double pos = player_.position();
        const bool atEnd = player_.eof() || (pos >= c->out - 0.5 * fd && pos < c->out + 5.0);
        if (atEnd) {
            const int idx = selectedIndex();
            if (kDebug)
                SDL_Log("[%.2fs] fin del clip %d: pos=%.3f out=%.3f eof=%d -> %s", SDL_GetTicks() / 1000.0, idx + 1, pos,
                        c->out, (int)player_.eof(), playAll_ && idx + 1 < (int)clips_.size() ? "siguiente" : "pausa");
            if (playAll_ && idx + 1 < (int)clips_.size()) {
                select(idx + 1, clips_[(size_t)idx + 1].in);
                player_.setPaused(false);
            } else {
                const bool wasEof = player_.eof();
                const double in = c->in, out = c->out;
                pause();
                if (!wasEof) player_.seek(std::max(in, out - fd));
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Editing

void App::setInAt(double t) {
    Clip* c = selectedClip();
    const MediaInfo* mi = c ? info(c->path) : nullptr;
    if (!mi) return;
    t = std::clamp(t, 0.0, mi->duration);
    Snapshot before = snapshot();
    if (t > c->out - kMinClip) c->out = mi->duration;  // a start after the end resets the end
    c->in = std::max(0.0, std::min(t, c->out - kMinClip));
    if (before.clips != clips_) pushUndo(std::move(before));
}

void App::setOutAt(double t) {
    Clip* c = selectedClip();
    const MediaInfo* mi = c ? info(c->path) : nullptr;
    if (!mi) return;
    t = std::clamp(t, 0.0, mi->duration);
    Snapshot before = snapshot();
    if (t < c->in + kMinClip) c->in = 0;  // an end before the start resets the start
    c->out = std::min(mi->duration, std::max(t, c->in + kMinClip));
    if (before.clips != clips_) pushUndo(std::move(before));
}

void App::splitAtPlayhead() {
    const int idx = selectedIndex();
    if (idx < 0) return;
    const double t = player_.position();
    const Clip c = clips_[(size_t)idx];
    if (t <= c.in + kMinClip || t >= c.out - kMinClip) {
        toast("Coloca el cursor dentro del recorte para dividir el clip");
        return;
    }
    pause();
    pushUndo();
    Clip second = c;
    second.id = nextId_++;
    second.in = t;
    clips_[(size_t)idx].out = t;
    clips_.insert(clips_.begin() + idx + 1, second);
    selectedId_ = second.id;  // same file and position: the player does not need to move
    scrollToSelected_ = true;
}

void App::removeClip(int index) {
    if (index < 0 || index >= (int)clips_.size()) return;
    pushUndo();
    const bool wasSelected = clips_[(size_t)index].id == selectedId_;
    clips_.erase(clips_.begin() + index);
    if (!wasSelected) return;
    pause();
    if (clips_.empty()) {
        select(-1, 0);
    } else {
        const int next = std::min(index, (int)clips_.size() - 1);
        select(next, clips_[(size_t)next].in);
    }
}

void App::duplicateClip(int index) {
    if (index < 0 || index >= (int)clips_.size()) return;
    pushUndo();
    Clip copy = clips_[(size_t)index];
    copy.id = nextId_++;
    clips_.insert(clips_.begin() + index + 1, copy);
}

void App::moveClip(int from, int to) {
    const int n = (int)clips_.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    pushUndo();
    Clip c = clips_[(size_t)from];
    clips_.erase(clips_.begin() + from);
    clips_.insert(clips_.begin() + to, c);
}

void App::pushUndo(Snapshot s) {
    undo_.push_back(std::move(s));
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
}

void App::restore(const Snapshot& s) {
    pause();
    clips_ = s.clips;
    int idx = indexOf(s.selected);
    if (idx < 0 && !clips_.empty()) idx = 0;
    if (idx < 0) {
        select(-1, 0);
        return;
    }
    const Clip& c = clips_[(size_t)idx];
    const double pos = player_.position();
    const bool keep = player_.path() == c.path && pos >= c.in && pos <= c.out;
    select(idx, keep ? pos : c.in);
}

void App::undo() {
    if (undo_.empty()) return;
    redo_.push_back(snapshot());
    Snapshot s = std::move(undo_.back());
    undo_.pop_back();
    restore(s);
}

void App::redo() {
    if (redo_.empty()) return;
    undo_.push_back(snapshot());
    Snapshot s = std::move(redo_.back());
    redo_.pop_back();
    restore(s);
}

void App::beginEdit() {
    editStart_ = snapshot();
    editing_ = true;
}

void App::endEdit() {
    if (!editing_) return;
    editing_ = false;
    if (editStart_.clips != clips_) pushUndo(std::move(editStart_));
}

// ---------------------------------------------------------------------------------------------
// Thumbnails

std::shared_ptr<App::Texture> App::thumbnailFor(const Clip& c) {
    const std::string key = thumbKey(c);
    auto it = thumbs_.find(key);
    if (it != thumbs_.end()) return it->second;
    if (drag_ == Drag::None && thumbRequests_.insert(key).second) {
        ++pendingJobs_;
        const std::string path = c.path;
        const double t = c.in;
        worker_.submit([this, key, path, t] {
            std::vector<unsigned char> rgba;
            const bool ok = media::thumbnail(path, t, kThumbW, kThumbH, rgba);
            mainQueue_.post([this, key, ok, rgba = std::move(rgba)] { onThumbnail(key, ok, rgba); });
        });
    }
    // Until the exact frame is ready, reuse any thumbnail of the same file.
    const std::string prefix = c.path + "|";
    auto lb = thumbs_.lower_bound(prefix);
    if (lb != thumbs_.end() && lb->first.compare(0, prefix.size(), prefix) == 0) return lb->second;
    return nullptr;
}

void App::onThumbnail(const std::string& key, bool ok, const std::vector<unsigned char>& rgba) {
    --pendingJobs_;
    if (!ok || rgba.size() < (size_t)kThumbW * kThumbH * 4) return;  // keep the request mark: no retry loop
    thumbRequests_.erase(key);

    auto tex = std::make_shared<Texture>();
    gl::GenTextures(1, &tex->id);
    gl::BindTexture(GL_TEXTURE_2D, tex->id);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl::PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kThumbW, kThumbH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    gl::BindTexture(GL_TEXTURE_2D, 0);
    thumbs_[key] = tex;

    if (thumbs_.size() > 128) {  // keep the cache small
        std::set<std::string> used;
        for (const Clip& c : clips_) used.insert(thumbKey(c));
        for (auto i = thumbs_.begin(); i != thumbs_.end();) i = used.count(i->first) ? std::next(i) : thumbs_.erase(i);
    }
}

// ---------------------------------------------------------------------------------------------
// Dialogs, export, notifications

void App::openAddDialog() {
    static const SDL_DialogFileFilter kFilters[] = {
        {"Videos", "mp4;mkv;mov;webm;avi;m4v;mpg;mpeg;ts;mts;m2ts;wmv;flv;3gp;ogv;gif"},
        {"Todos los archivos", "*"},
    };
    auto onDone = [](void*, const char* const* files, int) {
        if (!files) {
            const std::string err = SDL_GetError();
            App::postFromAnyThread([err](App& app) {
                app.toast("No se pudo abrir el selector de archivos (" + err + "). Arrastra los videos a la ventana.", true);
            });
            return;
        }
        std::vector<std::string> list;
        for (const char* const* f = files; *f; ++f) list.emplace_back(*f);
        if (!list.empty()) App::postFromAnyThread([list](App& app) { app.addFiles(list); });
    };
    SDL_ShowOpenFileDialog(onDone, nullptr, window_, kFilters, 2, lastDir_.empty() ? nullptr : lastDir_.c_str(), true);
}

void App::chooseExportPath() {
    static const SDL_DialogFileFilter kFilters[] = {{"Video MP4", "mp4"}, {"Video MKV", "mkv"}, {"Video MOV", "mov"}};
    auto onDone = [](void*, const char* const* files, int filter) {
        if (!files) {
            const std::string err = SDL_GetError();
            App::postFromAnyThread([err](App& app) { app.toast("No se pudo abrir el selector de archivos: " + err, true); });
            return;
        }
        if (!files[0]) return;  // cancelled
        std::string path = files[0];
        App::postFromAnyThread([path, filter](App& app) { app.setExportPath(path, filter); });
    };
    SDL_ShowSaveFileDialog(onDone, nullptr, window_, kFilters, 3, exportPath_.empty() ? nullptr : exportPath_.c_str());
}

void App::setExportPath(std::string path, int filter) {
    static const char* kExt[] = {"mp4", "mkv", "mov"};
    const std::string ext = util::extensionLower(path);
    if (ext != "mp4" && ext != "mkv" && ext != "mov") {
        path += '.';
        path += (filter >= 0 && filter < 3) ? kExt[filter] : "mp4";
    }
    exportPath_ = path;
    lastExportDir_ = util::dirName(path);
}

std::string App::defaultExportPath() const {
    const std::string dir = !lastExportDir_.empty() ? lastExportDir_ : util::dirName(clips_.front().path);
    const std::string base = util::stem(clips_.front().path) + "_editado";
    std::string candidate = util::joinPath(dir, base + ".mp4");
    for (int n = 2; util::pathExists(candidate) && n < 10000; ++n)
        candidate = util::joinPath(dir, util::format("%s_%d.mp4", base.c_str(), n));
    return candidate;
}

void App::openExportDialog() {
    if (clips_.empty()) {
        toast("Agrega al menos un video para exportar");
        return;
    }
    if (export_.state() != ExportJob::State::Running) {
        export_.reset();
        exportPath_ = defaultExportPath();
        exportRes_ = 0;
        exportFps_ = 0;
    }
    pause();
    exportRequested_ = true;
}

void App::startExport(int width, int height, int fpsNum, int fpsDen) {
    if (encoder_.empty()) return;
    for (const Clip& c : clips_) {
        if (util::isSameFile(c.path, exportPath_)) {
            toast("El archivo de salida no puede ser uno de los videos originales", true);
            return;
        }
    }
    std::vector<ExportClip> list;
    for (const Clip& c : clips_) {
        const MediaInfo* mi = info(c.path);
        list.push_back(ExportClip{c.path, c.in, c.out, (double)c.speed, mi && mi->hasAudio});
    }
    ExportSettings s;
    s.output = exportPath_;
    s.width = width;
    s.height = height;
    s.fpsNum = fpsNum;
    s.fpsDen = fpsDen;
    s.quality = exportQuality_;
    s.encoder = encoder_;
    lastExportDir_ = util::dirName(exportPath_);
    export_.start(buildExportArgs(media::ffmpegPath(), list, s), exportDuration(list), exportPath_);
}

bool App::requestQuit() {
    if (export_.state() != ExportJob::State::Running) return true;
    confirmQuit_ = true;
    return false;
}

void App::drawQuitDialog() {
    static const char* kTitle = "¿Salir?";
    if (confirmQuit_) {
        ImGui::OpenPopup(kTitle);
        confirmQuit_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) return;
    ImGui::TextUnformatted("Hay una exportación en curso. Si sales, se cancelará.");
    ImGui::Spacing();
    const char* stay = "Seguir exportando";
    const char* leave = "Cancelar y salir";
    rightAlign(ui::buttonWidth(stay) + ui::buttonWidth(leave) + ImGui::GetStyle().ItemSpacing.x);
    if (ui::primaryButton(stay) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        export_.state() != ExportJob::State::Running)
        ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button(leave)) {
        export_.cancel();
        quitNow_ = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::toast(const std::string& text, bool error) {
    toasts_.push_back(Toast{text, error, SDL_GetTicks() + (error ? 6000u : 3500u)});
    if (toasts_.size() > 4) toasts_.erase(toasts_.begin());
}

// ---------------------------------------------------------------------------------------------
// UI

void App::draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    const ImGuiStyle& st = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    const float fh = ImGui::GetFrameHeight();
    const float pad = st.WindowPadding.y;
    const float timelineH = std::round(em * 2.6f);
    const float editorH = pad * 2 + fh * 2 + timelineH + st.ItemSpacing.y * 2;
    const float cardW = std::round(em * 10.0f), cardPad = std::round(em * 0.4f);
    const float cardH = std::round(cardPad * 2.8f + (cardW - cardPad * 2) * 9.0f / 16.0f + ImGui::GetTextLineHeight() * 2 + 2);
    const float clipsH = pad * 2 + fh + st.ItemSpacing.y + cardH + st.ScrollbarSize + 4;

    drawTopBar();
    const float previewH = ImGui::GetContentRegionAvail().y - editorH - clipsH - st.ItemSpacing.y * 2;
    drawPreview(std::max(previewH, em * 6));
    drawEditor(editorH);
    drawClips(clipsH);
    ImGui::End();

    drawExportDialog();
    drawQuitDialog();
    drawToasts();
    handleShortcuts();

    // Structural changes requested by widgets run here, when nothing references clips_.
    std::vector<std::function<void()>> actions;
    actions.swap(deferred_);
    for (auto& action : actions) action();
}

void App::drawTopBar() {
    const float em = ImGui::GetFontSize();
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(ui::boldFont(), ui::kFontSize * 1.15f);
    ImGui::TextUnformatted("NebulaTrim");
    ImGui::PopFont();
    if (toolsChecked_ && encoder_.empty()) {
        ImGui::SameLine(0, em);
        ImGui::TextColored(ui::color::danger, "%s: instala FFmpeg o ponlo junto al programa",
                           toolsError_.empty() ? "FFmpeg no disponible" : toolsError_.c_str());
    }

    const char* addLabel = "Agregar videos";
    const char* exportLabel = "Exportar";
    ImGui::SameLine();
    rightAlign(ui::buttonWidth(addLabel) + ui::buttonWidth(exportLabel) + ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(addLabel)) defer([this] { openAddDialog(); });
    ImGui::SetItemTooltip("Agregar uno o varios videos  (Ctrl+O)");
    ImGui::SameLine();
    ImGui::BeginDisabled(clips_.empty());
    if (ui::primaryButton(exportLabel)) defer([this] { openExportDialog(); });
    ImGui::SetItemTooltip("Unir los clips y guardar el video final  (Ctrl+E)");
    ImGui::EndDisabled();
}

void App::drawPreview(float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::color::preview);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##preview", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    if (selectedClip() && player_.hasFile()) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 s(std::max(avail.x, 1.0f), std::max(avail.y, 1.0f));
        const ImVec2 fb = ImGui::GetIO().DisplayFramebufferScale;
        previewW_ = std::max(1, (int)std::lround(s.x * fb.x));
        previewH_ = std::max(1, (int)std::lround(s.y * fb.y));

        ImGui::InvisibleButton("##video", s);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) defer([this] { togglePlay(); });
        const bool hovered = ImGui::IsItemHovered();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddImageRounded((ImTextureID)player_.texture(), p, p + s, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE,
                            ImGui::GetStyle().ChildRounding);
        if (!playing_ && hovered) {
            const float r = ImGui::GetFontSize() * 1.8f;
            const ImVec2 center = p + s * 0.5f;
            dl->AddCircleFilled(center, r, IM_COL32(0, 0, 0, 140));
            ui::drawIcon(dl, ui::Icon::Play, center, r * 0.8f, IM_COL32(255, 255, 255, 230));
        }
    } else {
        previewW_ = previewH_ = 0;
        drawEmptyState();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::drawEmptyState() {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 s = ImGui::GetContentRegionAvail();
    if (s.x < 8 || s.y < 8) return;
    const float em = ImGui::GetFontSize();

    if (ImGui::InvisibleButton("##drop", s)) defer([this] { openAddDialog(); });
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 inset(em, em);
    dl->AddRect(p + inset, p + s - inset, hovered ? ui::u32(ui::color::accent, 0.8f) : IM_COL32(255, 255, 255, 28),
                em * 0.8f, 1.5f);

    const ImVec2 c = p + s * 0.5f;
    const float r = em * 1.9f;
    const ImVec2 iconCenter(c.x, c.y - em * 2.4f);
    dl->AddCircleFilled(iconCenter, r, ui::u32(ui::color::accent, hovered ? 1.0f : 0.85f));
    ui::drawIcon(dl, ui::Icon::Plus, iconCenter, r * 0.8f, IM_COL32_WHITE);

    auto centered = [&](const char* text, float y, ImU32 col) {
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(std::round(c.x - ts.x * 0.5f), std::round(y)), col, text);
    };
    ImGui::PushFont(ui::boldFont(), ui::kFontSize * 1.25f);
    centered("Arrastra tus videos aquí", c.y, ui::u32(ui::color::text));
    ImGui::PopFont();
    centered("o haz clic para elegirlos", c.y + em * 1.9f, ui::u32(ui::color::textDim));
    centered("MP4 · MKV · MOV · WEBM · AVI y más", c.y + em * 3.3f, ui::u32(ui::color::textDim, 0.7f));
}

void App::drawEditor(float height) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    ImGui::BeginChild("##editor", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    Clip* c = selectedClip();
    const MediaInfo* mi = c ? info(c->path) : nullptr;
    const bool enabled = c && mi;
    ImGui::BeginDisabled(!enabled);

    // Row 1: transport + trim actions
    const double pos = enabled ? player_.position() : 0.0;
    if (ui::iconButton("##stepback", ui::Icon::StepBack, "Fotograma anterior  (←)")) defer([this] { stepFrame(false); });
    ImGui::SameLine(0, st.ItemSpacing.x * 0.5f);
    if (ui::iconButton("##play", playing_ ? ui::Icon::Pause : ui::Icon::Play, "Reproducir / pausar  (Espacio)", true))
        defer([this] { togglePlay(); });
    ImGui::SameLine(0, st.ItemSpacing.x * 0.5f);
    if (ui::iconButton("##stepfwd", ui::Icon::StepForward, "Fotograma siguiente  (→)")) defer([this] { stepFrame(true); });
    ImGui::SameLine(0, em);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(util::formatTime(pos).c_str());
    ImGui::SameLine(0, em * 0.4f);
    ImGui::TextDisabled("/ %s", util::formatTime(enabled ? mi->duration : 0.0).c_str());

    const char* inLabel = "Inicio aquí";
    const char* outLabel = "Fin aquí";
    const char* splitLabel = "Dividir";
    ImGui::SameLine();
    rightAlign(ui::buttonWidth(inLabel) + ui::buttonWidth(outLabel) + ui::buttonWidth(splitLabel) + st.ItemSpacing.x * 2);
    if (ImGui::Button(inLabel)) defer([this] { setInAt(player_.position()); });
    ImGui::SetItemTooltip("Recorta el inicio del clip en la posición actual  (I)");
    ImGui::SameLine();
    if (ImGui::Button(outLabel)) defer([this] { setOutAt(player_.position()); });
    ImGui::SetItemTooltip("Recorta el final del clip en la posición actual  (O)");
    ImGui::SameLine();
    if (ImGui::Button(splitLabel)) defer([this] { splitAtPlayhead(); });
    ImGui::SetItemTooltip("Divide el clip en dos en la posición actual  (S)");

    // Row 2: timeline
    const float timelineH = std::round(em * 2.6f);
    if (enabled) {
        drawTimeline(*c, *mi, timelineH);
    } else {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, hw = std::round(em * 0.6f);
        ImGui::Dummy(ImVec2(w, timelineH));
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p0.x + hw, std::round(p0.y + em * 0.45f)),
                                                  ImVec2(p0.x + w - hw, std::round(p0.y + timelineH - em * 0.45f)),
                                                  ui::u32(ui::color::track, 0.5f), std::round(em * 0.3f));
    }

    // Row 3: speed
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Velocidad");
    ImGui::SameLine();
    std::string summary;
    if (enabled) {
        const double srcLen = c->out - c->in;
        summary = util::format("Recorte %s – %s  ·  ", util::formatTime(c->in).c_str(), util::formatTime(c->out).c_str());
        if (std::fabs(c->speed - 1.0f) > 1e-4f)
            summary += util::format("%.2f s → %.2f s", srcLen, srcLen / c->speed);
        else
            summary += util::format("%.2f s", srcLen);
    }
    const char* resetLabel = "1×";
    const float summaryW = ImGui::CalcTextSize(summary.c_str()).x;
    const float sliderW = std::max(em * 8.0f, ImGui::GetContentRegionAvail().x - ui::buttonWidth(resetLabel) - summaryW -
                                                  st.ItemSpacing.x * 2 - em);
    ImGui::SetNextItemWidth(sliderW);
    float speed = enabled ? c->speed : 1.0f;
    if (ImGui::SliderFloat("##speed", &speed, 0.1f, 10.0f, "%.2f×",
                           ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp) &&
        enabled) {
        c->speed = std::clamp(speed, 0.1f, 10.0f);
        player_.setSpeed(c->speed);
    }
    if (ImGui::IsItemActivated()) beginEdit();
    if (ImGui::IsItemDeactivated()) endEdit();
    ImGui::SetItemTooltip("Arrastra para cambiar la velocidad (0.1× a 10×).\nCtrl+clic para escribir un valor exacto.");
    ImGui::SameLine();
    if (ImGui::Button(resetLabel) && enabled && c->speed != 1.0f) {
        pushUndo();
        c->speed = 1.0f;
        player_.setSpeed(1.0);
    }
    ImGui::SetItemTooltip("Velocidad normal");
    if (enabled) {
        ImGui::SameLine();
        rightAlign(summaryW);
        ImGui::TextDisabled("%s", summary.c_str());
    }

    ImGui::EndDisabled();
    ImGui::EndChild();
}

void App::drawTimeline(Clip& c, const MediaInfo& mi, float height) {
    const float em = ImGui::GetFontSize();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##timeline", ImVec2(std::max(w, 1.0f), height));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    const float handleW = std::round(em * 0.6f);
    const float x0 = p0.x + handleW, x1 = std::max(x0 + 1.0f, p0.x + w - handleW);
    const double dur = std::max(mi.duration, 0.001);
    auto tToX = [&](double t) { return x0 + (float)std::clamp(t / dur, 0.0, 1.0) * (x1 - x0); };
    auto xToT = [&](float x) { return std::clamp((double)((x - x0) / (x1 - x0)), 0.0, 1.0) * dur; };
    const float yTop = std::round(p0.y + em * 0.45f), yBot = std::round(p0.y + height - em * 0.45f);
    const float xi = std::round(tToX(c.in)), xo = std::round(tToX(c.out));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    auto nearIn = [&](float mx) { return mx >= xi - handleW - 3 && mx <= xi + 4; };
    auto nearOut = [&](float mx) { return mx >= xo - 4 && mx <= xo + handleW + 3; };

    if (ImGui::IsItemActivated()) {
        const float mx = ImGui::GetIO().MouseClickedPos[0].x;
        const bool ni = nearIn(mx), no = nearOut(mx);
        if (ni && no)
            drag_ = mx < (xi + xo) * 0.5f ? Drag::In : Drag::Out;
        else
            drag_ = ni ? Drag::In : no ? Drag::Out : Drag::Scrub;
        resumeAfterScrub_ = playing_ && drag_ == Drag::Scrub;
        if (playing_) pause();
        if (drag_ != Drag::Scrub) beginEdit();
        lastSeekTarget_ = -1;
    }
    if (active && drag_ != Drag::None) {
        const double t = xToT(mouse.x);
        double target = t;
        if (drag_ == Drag::In) {
            c.in = std::clamp(t, 0.0, std::max(0.0, c.out - kMinClip));
            target = c.in;
        } else if (drag_ == Drag::Out) {
            c.out = std::clamp(t, std::min(mi.duration, c.in + kMinClip), mi.duration);
            target = std::max(c.in, c.out - 1.0 / mi.fps());  // last frame kept
        }
        if (std::fabs(target - lastSeekTarget_) > 0.5 / mi.fps()) {
            player_.seek(target);
            lastSeekTarget_ = target;
        }
    }
    if (ImGui::IsItemDeactivated()) {
        if (drag_ == Drag::In || drag_ == Drag::Out) endEdit();
        if (drag_ == Drag::Scrub && resumeAfterScrub_) defer([this] { togglePlay(); });
        drag_ = Drag::None;
    }
    const bool handleHot = drag_ == Drag::In || drag_ == Drag::Out || (hovered && (nearIn(mouse.x) || nearOut(mouse.x)));
    if (handleHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = std::round(em * 0.3f);
    const ImU32 accent = ui::u32(handleHot ? ui::color::accentHover : ui::color::accent);
    dl->AddRectFilled(ImVec2(x0, yTop), ImVec2(x1, yBot), ui::u32(ui::color::track), r);
    dl->AddRectFilled(ImVec2(xi, yTop), ImVec2(xo, yBot), ui::u32(ui::color::accent, 0.25f));
    dl->AddLineH(xi, xo, yTop, accent, 2.0f);
    dl->AddLineH(xi, xo, yBot - 2.0f, accent, 2.0f);
    dl->AddRectFilled(ImVec2(xi - handleW, yTop - 2), ImVec2(xi, yBot + 2), accent, r, ImDrawFlags_RoundLeft);
    dl->AddRectFilled(ImVec2(xo, yTop - 2), ImVec2(xo + handleW, yBot + 2), accent, r, ImDrawFlags_RoundRight);
    const float gy0 = std::round((yTop + yBot) * 0.5f - em * 0.35f), gy1 = std::round(gy0 + em * 0.7f);
    dl->AddLineV(std::round(xi - handleW * 0.5f), gy0, gy1, IM_COL32(255, 255, 255, 190), 1.0f);
    dl->AddLineV(std::round(xo + handleW * 0.5f) - 1, gy0, gy1, IM_COL32(255, 255, 255, 190), 1.0f);

    const float xp = std::round(tToX(player_.position()));
    dl->AddLineV(xp - 1, p0.y + em * 0.2f, p0.y + height - em * 0.1f, IM_COL32(255, 255, 255, 235), 2.0f);
    dl->AddCircleFilled(ImVec2(xp, p0.y + em * 0.25f), em * 0.24f, IM_COL32(255, 255, 255, 245));

    if (hovered && !active && !handleHot) {
        ImGui::SetTooltip("%s", util::formatTime(xToT(mouse.x)).c_str());
    }
}

void App::drawClips(float height) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    ImGui::BeginChild("##clips", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(ui::boldFont(), 0.0f);
    ImGui::TextUnformatted("Clips");
    ImGui::PopFont();
    ImGui::SameLine();
    if (clips_.empty())
        ImGui::TextDisabled("Aún no hay clips");
    else
        ImGui::TextDisabled("%d %s  ·  duración final %s", (int)clips_.size(), clips_.size() == 1 ? "clip" : "clips",
                            util::formatTime(totalDuration()).c_str());

    const char* playAllLabel = playAll_ ? "Detener" : "Reproducir todo";
    ImGui::SameLine();
    rightAlign(ui::buttonWidth(playAllLabel));
    ImGui::BeginDisabled(clips_.empty());
    if (ImGui::Button(playAllLabel)) defer([this] { togglePlayAll(); });
    ImGui::SetItemTooltip("Reproduce todos los clips en orden, con sus recortes y velocidades");
    ImGui::EndDisabled();

    const float cardW = std::round(em * 10.0f), cardPad = std::round(em * 0.4f);
    const float cardH = std::round(cardPad * 2.8f + (cardW - cardPad * 2) * 9.0f / 16.0f + ImGui::GetTextLineHeight() * 2 + 2);
    const float rounding = st.FrameRounding + 2;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##strip", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool first = true;
    for (size_t i = 0; i < clips_.size(); ++i) {
        if (!first) ImGui::SameLine();
        first = false;
        ImGui::PushID((int)clips_[i].id);
        drawCard((int)i, clips_[i], cardW, cardH);
        ImGui::PopID();
    }
    for (size_t i = 0; i < loading_.size(); ++i) {  // files still being analyzed
        if (!first) ImGui::SameLine();
        first = false;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(cardW, cardH));
        const ImVec2 tp(p.x + cardPad, p.y + cardPad);
        const ImVec2 ts(cardW - cardPad * 2, (cardW - cardPad * 2) * 9.0f / 16.0f);
        dl->AddRectFilled(p, p + ImVec2(cardW, cardH), ui::u32(ui::color::card), rounding);
        dl->AddRectFilled(tp, tp + ts, ui::u32(ui::color::preview), rounding * 0.7f);
        const ImVec2 ls = ImGui::CalcTextSize("Cargando…");
        dl->AddText(tp + (ts - ls) * 0.5f, ui::u32(ui::color::textDim), "Cargando…");
        ui::textEllipsis(dl, ImVec2(p.x + cardPad, tp.y + ts.y + cardPad * 0.8f), ui::u32(ui::color::textDim),
                         util::fileName(loading_[i]).c_str(), cardW - cardPad * 2);
    }
    {  // "add" card
        if (!first) ImGui::SameLine();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##addcard", ImVec2(cardW, cardH))) defer([this] { openAddDialog(); });
        const bool hov = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("Agregar videos  (Ctrl+O)");
        dl->AddRect(p, p + ImVec2(cardW, cardH), hov ? ui::u32(ui::color::accent) : IM_COL32(255, 255, 255, 30), rounding,
                    1.5f);
        const ImVec2 center(p.x + cardW * 0.5f, p.y + cardH * 0.42f);
        ui::drawIcon(dl, ui::Icon::Plus, center, em * 1.3f, hov ? ui::u32(ui::color::accentHover) : ui::u32(ui::color::textDim));
        const ImVec2 ls = ImGui::CalcTextSize("Agregar");
        dl->AddText(ImVec2(std::round(center.x - ls.x * 0.5f), std::round(center.y + em * 1.2f)),
                    hov ? ui::u32(ui::color::text) : ui::u32(ui::color::textDim), "Agregar");
    }

    // Vertical mouse wheel scrolls the strip horizontally.
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f && io.MouseWheelH == 0.0f)
        ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseWheel * em * 6.0f);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndChild();
}

void App::drawCard(int index, const Clip& c, float w, float h) {
    const float em = ImGui::GetFontSize();
    const float pad = std::round(em * 0.4f);
    const float rounding = ImGui::GetStyle().FrameRounding + 2;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(w, h);
    const bool selected = c.id == selectedId_;
    const uint64_t id = c.id;
    const std::string name = util::fileName(c.path);

    ImGui::BeginGroup();
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##card", size);
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p, p + size);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !selected) {
        defer([this, id] {
            const int i = indexOf(id);
            if (i < 0) return;
            pause();
            select(i, clips_[(size_t)i].in, false);
        });
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("NT_CLIP", &index, sizeof(index));
        ImGui::Text("Mover «%s»", name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NT_CLIP")) {
            const int from = *static_cast<const int*>(payload->Data);
            defer([this, from, index] { moveClip(from, index); });
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##menu")) {
        if (ImGui::MenuItem("Duplicar", "Ctrl+D")) defer([this, index] { duplicateClip(index); });
        if (ImGui::MenuItem("Mover a la izquierda", nullptr, false, index > 0))
            defer([this, index] { moveClip(index, index - 1); });
        if (ImGui::MenuItem("Mover a la derecha", nullptr, false, index + 1 < (int)clips_.size()))
            defer([this, index] { moveClip(index, index + 1); });
        ImGui::Separator();
        if (ImGui::MenuItem("Quitar", "Supr")) defer([this, index] { removeClip(index); });
        ImGui::EndPopup();
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4& bg = selected ? ui::color::cardSelected : hovered ? ui::color::cardHover : ui::color::card;
    dl->AddRectFilled(p, p + size, ui::u32(bg), rounding);

    const ImVec2 tp(p.x + pad, p.y + pad);
    const ImVec2 ts(w - pad * 2, (w - pad * 2) * 9.0f / 16.0f);
    if (auto tex = thumbnailFor(c))
        dl->AddImageRounded((ImTextureID)tex->id, tp, tp + ts, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rounding * 0.7f);
    else
        dl->AddRectFilled(tp, tp + ts, ui::u32(ui::color::preview), rounding * 0.7f);

    // Position badge (top-left) and speed badge (bottom-right).
    char number[16];
    std::snprintf(number, sizeof(number), "%d", index + 1);
    const ImVec2 ns = ImGui::CalcTextSize(number);
    const ImVec2 b0(tp.x + em * 0.3f, tp.y + em * 0.3f);
    dl->AddRectFilled(b0, b0 + ImVec2(ns.x + em * 0.7f, ns.y + em * 0.2f), IM_COL32(0, 0, 0, 175), em * 0.35f);
    dl->AddText(b0 + ImVec2(em * 0.35f, em * 0.1f), IM_COL32_WHITE, number);
    if (std::fabs(c.speed - 1.0f) > 1e-4f) {
        const std::string sp = util::format("%.2f×", c.speed);
        const ImVec2 ss = ImGui::CalcTextSize(sp.c_str());
        const ImVec2 s1(tp.x + ts.x - em * 0.3f, tp.y + ts.y - em * 0.3f);
        const ImVec2 s0 = s1 - ImVec2(ss.x + em * 0.7f, ss.y + em * 0.2f);
        dl->AddRectFilled(s0, s1, ui::u32(ui::color::accent, 0.95f), em * 0.35f);
        dl->AddText(s0 + ImVec2(em * 0.35f, em * 0.1f), IM_COL32_WHITE, sp.c_str());
    }

    const float ty = tp.y + ts.y + pad * 0.8f;
    ui::textEllipsis(dl, ImVec2(p.x + pad, ty), ui::u32(ui::color::text), name.c_str(), w - pad * 2);
    const std::string meta = util::formatTime((c.out - c.in) / c.speed);
    dl->AddText(ImVec2(p.x + pad, ty + ImGui::GetTextLineHeight() + 2), ui::u32(ui::color::textDim), meta.c_str());
    if (selected) dl->AddRect(p, p + size, ui::u32(ui::color::accent), rounding, 2.0f);

    if (hovered) {  // remove button
        const float bs = std::round(em * 1.35f);
        const ImVec2 bp(p.x + w - bs - em * 0.3f, p.y + em * 0.3f);
        ImGui::SetCursorScreenPos(bp);
        if (ImGui::InvisibleButton("##remove", ImVec2(bs, bs))) defer([this, index] { removeClip(index); });
        const bool xh = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("Quitar del proyecto  (Supr)");
        const ImVec2 bc = bp + ImVec2(bs, bs) * 0.5f;
        dl->AddCircleFilled(bc, bs * 0.5f, xh ? ui::u32(ui::color::danger) : IM_COL32(0, 0, 0, 175));
        ui::drawIcon(dl, ui::Icon::Close, bc, bs * 0.36f, IM_COL32_WHITE);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(size);
    ImGui::EndGroup();

    if (selected && scrollToSelected_) {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const ImVec2 wmn = ImGui::GetWindowPos(), wmx = wmn + ImGui::GetWindowSize();
        if (mn.x < wmn.x || mx.x > wmx.x) ImGui::SetScrollHereX(0.5f);
        scrollToSelected_ = false;
    }
}

void App::drawExportSettings() {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    const MediaInfo* first = clips_.empty() ? nullptr : info(clips_.front().path);
    if (!first) {
        ImGui::CloseCurrentPopup();
        return;
    }

    struct Option {
        std::string label;
        int a, b;
    };
    std::vector<Option> res, fps;
    const int ow = evenSize(first->width), oh = evenSize(first->height);
    res.push_back({util::format("Igual al primer clip (%d×%d)", ow, oh), ow, oh});
    for (int target : {1080, 720, 480}) {
        int w, h;
        if (first->width >= first->height) {
            h = target;
            w = evenSize(target * (double)first->width / first->height);
        } else {
            w = target;
            h = evenSize(target * (double)first->height / first->width);
        }
        if (w == ow && h == oh) continue;
        res.push_back({util::format("%dp (%d×%d)", target, w, h), w, h});
    }
    fps.push_back({first->fpsDen == 1 ? util::format("Igual al primer clip (%d fps)", first->fpsNum)
                                      : util::format("Igual al primer clip (%.2f fps)", first->fps()),
                   first->fpsNum, first->fpsDen});
    for (int f : {24, 30, 60})
        if (!(first->fpsDen == 1 && first->fpsNum == f)) fps.push_back({util::format("%d fps", f), f, 1});
    static const char* kQuality[] = {"Alta (archivo más grande)", "Normal", "Ligera (archivo más pequeño)"};
    exportRes_ = std::clamp(exportRes_, 0, (int)res.size() - 1);
    exportFps_ = std::clamp(exportFps_, 0, (int)fps.size() - 1);
    exportQuality_ = std::clamp(exportQuality_, 0, 2);

    ImGui::TextDisabled("Guardar como");
    const char* changeLabel = "Cambiar…";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui::buttonWidth(changeLabel) - st.ItemSpacing.x);
    std::string shown = exportPath_;
    ImGui::InputText("##path", shown.data(), shown.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button(changeLabel)) chooseExportPath();
    ImGui::Spacing();

    const float labelW = em * 6.5f;
    auto combo = [&](const char* label, const char* id, int& value, int count, auto&& nameOf) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(id, nameOf(value))) {
            for (int i = 0; i < count; ++i) {
                const bool sel = i == value;
                if (ImGui::Selectable(nameOf(i), sel)) value = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    };
    combo("Resolución", "##res", exportRes_, (int)res.size(), [&](int i) { return res[(size_t)i].label.c_str(); });
    combo("FPS", "##fps", exportFps_, (int)fps.size(), [&](int i) { return fps[(size_t)i].label.c_str(); });
    combo("Calidad", "##quality", exportQuality_, 3, [&](int i) { return kQuality[i]; });

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%d %s  ·  duración final %s", (int)clips_.size(), clips_.size() == 1 ? "clip" : "clips",
                        util::formatTime(totalDuration()).c_str());
    if (!toolsChecked_)
        ImGui::TextDisabled("Comprobando FFmpeg…");
    else if (encoder_.empty())
        ImGui::TextColored(ui::color::danger, "%s", toolsError_.empty() ? "FFmpeg no está disponible" : toolsError_.c_str());
    ImGui::Spacing();

    const char* cancelLabel = "Cancelar";
    const char* goLabel = "Exportar";
    rightAlign(ui::buttonWidth(cancelLabel) + ui::buttonWidth(goLabel) + st.ItemSpacing.x);
    if (ImGui::Button(cancelLabel)) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    ImGui::BeginDisabled(encoder_.empty());
    if (ui::primaryButton(goLabel)) {
        const Option& r = res[(size_t)exportRes_];
        const Option& f = fps[(size_t)exportFps_];
        startExport(r.a, r.b, f.a, f.b);
    }
    ImGui::EndDisabled();
}

void App::drawExportDialog() {
    static const char* kTitle = "Exportar video";
    if (exportRequested_) {
        ImGui::OpenPopup(kTitle);
        exportRequested_ = false;
    }
    const ImGuiStyle& st = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 34.0f, 0.0f));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.2f, em * 1.0f));
    const bool open =
        ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!open) return;

    const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    switch (export_.state()) {
    case ExportJob::State::Idle:
        drawExportSettings();
        if (escape) ImGui::CloseCurrentPopup();
        break;
    case ExportJob::State::Running: {
        const float progress = export_.progress();
        ImGui::TextUnformatted("Exportando…");
        ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 0), util::format("%d %%", (int)(progress * 100)).c_str());
        const double elapsed = export_.elapsedSeconds();
        if (progress > 0.02f)
            ImGui::TextDisabled("Transcurrido %s  ·  faltan ~%s", util::formatTime(elapsed, 0).c_str(),
                                util::formatTime(elapsed / progress - elapsed, 0).c_str());
        else
            ImGui::TextDisabled("Transcurrido %s", util::formatTime(elapsed, 0).c_str());
        ImGui::Spacing();
        const char* cancelLabel = "Cancelar exportación";
        rightAlign(ui::buttonWidth(cancelLabel));
        if (ImGui::Button(cancelLabel)) export_.cancel();
        break;
    }
    case ExportJob::State::Done: {
        ImGui::TextColored(ui::color::success, "¡Listo! Tu video se exportó correctamente.");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", exportPath_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("Tiempo de exportación: %s", util::formatTime(export_.elapsedSeconds(), 0).c_str());
        ImGui::Spacing();
        const char* playLabel = "Abrir video";
        const char* dirLabel = "Abrir carpeta";
        const char* closeLabel = "Cerrar";
        rightAlign(ui::buttonWidth(playLabel) + ui::buttonWidth(dirLabel) + ui::buttonWidth(closeLabel) + st.ItemSpacing.x * 2);
        if (ImGui::Button(playLabel)) SDL_OpenURL(exportPath_.c_str());
        ImGui::SameLine();
        if (ImGui::Button(dirLabel)) SDL_OpenURL(util::dirName(exportPath_).c_str());
        ImGui::SameLine();
        if (ui::primaryButton(closeLabel) || escape) {
            export_.reset();
            ImGui::CloseCurrentPopup();
        }
        break;
    }
    case ExportJob::State::Failed: {
        ImGui::TextColored(ui::color::danger, "No se pudo exportar el video.");
        const std::string log = export_.log();
        ImGui::BeginChild("##log", ImVec2(0, em * 8.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(log.c_str());
        ImGui::EndChild();
        ImGui::Spacing();
        const char* copyLabel = "Copiar detalles";
        const char* closeLabel = "Cerrar";
        rightAlign(ui::buttonWidth(copyLabel) + ui::buttonWidth(closeLabel) + st.ItemSpacing.x);
        if (ImGui::Button(copyLabel)) SDL_SetClipboardText(log.c_str());
        ImGui::SameLine();
        if (ImGui::Button(closeLabel) || escape) {
            export_.reset();
            ImGui::CloseCurrentPopup();
        }
        break;
    }
    case ExportJob::State::Cancelled:
        export_.reset();
        ImGui::CloseCurrentPopup();
        toast("Exportación cancelada");
        break;
    }
    ImGui::EndPopup();
}

void App::drawToasts() {
    if (toasts_.empty()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float em = ImGui::GetFontSize();
    float y = vp->WorkPos.y + vp->WorkSize.y - em;
    for (int i = (int)toasts_.size() - 1; i >= 0; --i) {
        const Toast& t = toasts_[(size_t)i];
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - em, y), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, t.error ? ImVec4(0.32f, 0.11f, 0.15f, 0.97f) : ImVec4(0.17f, 0.17f, 0.21f, 0.97f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, em * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.9f, em * 0.6f));
        char name[32];
        std::snprintf(name, sizeof(name), "##toast%d", i);
        ImGui::Begin(name, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoMove);
        ImGui::PushTextWrapPos(em * 26.0f);
        ImGui::TextUnformatted(t.text.c_str());
        ImGui::PopTextWrapPos();
        y -= ImGui::GetWindowHeight() + em * 0.5f;
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
}

void App::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;

    const int sel = selectedIndex();
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_O, false)) defer([this] { openAddDialog(); });
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) defer([this] { openExportDialog(); });
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) defer([this, shift = io.KeyShift] { shift ? redo() : undo(); });
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) defer([this] { redo(); });
        if (ImGui::IsKeyPressed(ImGuiKey_D, false) && sel >= 0) defer([this, sel] { duplicateClip(sel); });
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_K, false))
        defer([this] { togglePlay(); });
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
        defer([this, shift = io.KeyShift] { shift ? seekRelative(-1.0) : stepFrame(false); });
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
        defer([this, shift = io.KeyShift] { shift ? seekRelative(1.0) : stepFrame(true); });
    if (sel < 0) return;
    const Clip c = clips_[(size_t)sel];
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) defer([this, c] { pause(); player_.seek(c.in); });
    if (ImGui::IsKeyPressed(ImGuiKey_End, false))
        defer([this, c] {
            pause();
            player_.seek(std::max(c.in, c.out - frameDuration(c)));
        });
    if (ImGui::IsKeyPressed(ImGuiKey_I, false)) defer([this] { setInAt(player_.position()); });
    if (ImGui::IsKeyPressed(ImGuiKey_O, false)) defer([this] { setOutAt(player_.position()); });
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) defer([this] { splitAtPlayhead(); });
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
        defer([this, sel] { removeClip(sel); });
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) && sel > 0)
        defer([this, sel] {
            pause();
            select(sel - 1, clips_[(size_t)sel - 1].in);
        });
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && sel + 1 < (int)clips_.size())
        defer([this, sel] {
            pause();
            select(sel + 1, clips_[(size_t)sel + 1].in);
        });
}
