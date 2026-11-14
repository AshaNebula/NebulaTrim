// Application state and UI.
#pragma once

#include "exporter.h"
#include "media.h"
#include "util.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct SDL_Window;
class Player;

struct Clip {
    uint64_t id = 0;
    std::string path;
    double in = 0, out = 0;  // trimmed range inside the source file (seconds)
    float speed = 1.0f;

    bool operator==(const Clip& o) const {
        return id == o.id && path == o.path && in == o.in && out == o.out && speed == o.speed;
    }
};

class App {
public:
    App(SDL_Window* window, Player& player);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    void addFiles(const std::vector<std::string>& paths);
    void update();  // every loop iteration: async results, player events, playback rules
    void draw();    // builds the ImGui frame
    void previewSize(int& w, int& h) const {
        w = previewW_;
        h = previewH_;
    }
    int waitTimeoutMs() const;  // how long the main loop may sleep (-1 = until the next event)
    bool requestQuit();         // false = asked the user to confirm (export running)
    bool quitNow() const { return quitNow_; }
    bool busy() const;          // background work still pending
    void openExportDialog();
    // Test hooks used by the NEBULATRIM_SNAPSHOT mode.
    void debugSeek(double seconds);
    void debugTrim(double in, double out, float speed);
    void debugSplit(double seconds);
    void debugPlay(int mode);  // 0 = selected clip, 1 = whole video, 2 = video from the playhead
    void debugDump() const;
    // Screen position of a UI spot from the last frame ("cut", "video", "clip-in", "clip-out").
    bool debugPoint(const std::string& name, float& x, float& y) const;

    // Thread-safe: runs `fn` on the main thread if the app still exists (used by file dialogs).
    static void postFromAnyThread(std::function<void(App&)> fn);

private:
    struct Texture;
    struct Snapshot {
        std::vector<Clip> clips;
        uint64_t selected = 0;
    };
    struct Toast {
        std::string text;
        bool error = false;
        uint64_t until = 0;
    };
    // Scrub/In/Out: clip bar. VideoScrub/Cut: whole-video bar.
    enum class Drag { None, Scrub, In, Out, VideoScrub, Cut };

    // Queries
    int indexOf(uint64_t id) const;
    int selectedIndex() const { return indexOf(selectedId_); }
    Clip* selectedClip();
    const MediaInfo* info(const std::string& path) const;
    double frameDuration(const Clip& c) const;
    double clipDuration(const Clip& c) const { return (c.out - c.in) / c.speed; }  // length in the final video
    double totalDuration() const;
    // Clip `index` and the next one are consecutive parts of the same file (no gap, no overlap).
    bool joined(int index) const;

    // Actions
    void defer(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }
    void addClip(const std::string& path, const MediaInfo& mi);
    void select(int index, double position, bool scrollIntoView = true);
    void togglePlay();
    void pause();
    // Plays the final video, from the beginning or from the playhead (continuing with the next clips).
    void togglePlayAll(bool fromHere);
    void seekRelative(double delta);
    void stepFrame(bool forward);
    void setInAt(double t);
    void setOutAt(double t);
    void splitAtPlayhead();
    void removeClip(int index);
    void duplicateClip(int index);
    void moveClip(int from, int to);
    void openAddDialog();
    void chooseExportPath();
    void setExportPath(std::string path, int filter);
    std::string defaultExportPath() const;
    void startExport(int width, int height, int fpsNum, int fpsDen);
    void toast(const std::string& text, bool error = false);

    // Undo / redo
    Snapshot snapshot() const { return Snapshot{clips_, selectedId_}; }
    void pushUndo(Snapshot s);
    void pushUndo() { pushUndo(snapshot()); }
    void restore(const Snapshot& s);
    void undo();
    void redo();
    void beginEdit();
    void endEdit();

    // Async results
    void onProbed(const std::string& path, const MediaInfo& mi);
    void onThumbnail(const std::string& key, bool ok, const std::vector<unsigned char>& rgba);
    std::shared_ptr<Texture> thumbnailFor(const Clip& c);

    // UI
    void drawTopBar();
    void drawPreview(float height);
    void drawEmptyState();
    void drawEditor(float height);
    void drawTimeline(Clip& c, const MediaInfo& mi, float height);
    void drawVideoBar(float height);
    void drawClips(float height);
    void drawCard(int index, const Clip& c, float w, float h);
    void drawExportDialog();
    void drawExportSettings();
    void drawQuitDialog();
    void drawToasts();
    void handleShortcuts();

    SDL_Window* window_;
    Player& player_;
    MainQueue mainQueue_;
    Worker worker_;  // declared after mainQueue_ so it is joined first

    std::vector<Clip> clips_;
    uint64_t selectedId_ = 0;
    uint64_t nextId_ = 1;
    std::map<std::string, MediaInfo> media_;
    std::vector<std::string> loading_;
    std::map<std::string, std::shared_ptr<Texture>> thumbs_;
    std::set<std::string> thumbRequests_;
    int pendingJobs_ = 0;
    std::string lastDir_;

    std::vector<Snapshot> undo_, redo_;
    Snapshot editStart_;
    bool editing_ = false;

    bool playing_ = false;
    bool playAll_ = false;
    bool playAllFromHere_ = false;  // the sequence playback started at the playhead
    Drag drag_ = Drag::None;
    float dragX_ = 0;           // virtual mouse x while dragging (Shift = 10x finer)
    float dragOriginX_ = 0;
    bool dragMoved_ = false;    // edits only start once the mouse actually moves
    double snapTime_ = -1;      // edge the clip bar handle snapped to (shown while dragging)
    uint64_t cutLeft_ = 0, cutRight_ = 0;  // clips on each side of the cut being moved
    double cutStart_ = 0;       // where the left clip starts in the final video
    double cutScale_ = 0;       // video length frozen while a cut is moved (stable mapping)
    bool resumeAfterScrub_ = false;
    double lastSeekTarget_ = -1;
    std::map<std::string, std::pair<float, float>> debugPoints_;
    bool scrollToSelected_ = false;
    std::vector<std::function<void()>> deferred_;

    ExportJob export_;
    bool exportRequested_ = false;
    std::string exportPath_;
    std::string lastExportDir_;
    int exportRes_ = 0, exportFps_ = 0, exportQuality_ = 1;
    std::string encoder_;
    std::string toolsError_;
    bool toolsChecked_ = false;

    bool confirmQuit_ = false;
    bool quitNow_ = false;

    std::vector<Toast> toasts_;
    int previewW_ = 0, previewH_ = 0;
};
