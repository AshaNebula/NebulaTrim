// Builds the ffmpeg command that renders the final video and runs it in a background thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ExportClip {
    std::string path;
    double in = 0, out = 0;  // source range in seconds
    double speed = 1;
    bool hasAudio = false;
};

struct ExportSettings {
    std::string output;
    int width = 1920, height = 1080;  // must be even
    int fpsNum = 30, fpsDen = 1;
    int quality = 1;  // 0 = high, 1 = normal, 2 = small file
    std::string encoder = "libx264";
};

double exportDuration(const std::vector<ExportClip>& clips);
std::vector<std::string> buildExportArgs(const std::string& ffmpeg, const std::vector<ExportClip>& clips,
                                         const ExportSettings& s);

class ExportJob {
public:
    enum class State { Idle, Running, Done, Failed, Cancelled };

    ExportJob() = default;
    ~ExportJob();
    ExportJob(const ExportJob&) = delete;
    ExportJob& operator=(const ExportJob&) = delete;

    void start(std::vector<std::string> args, double totalSeconds, std::string outputPath);
    void cancel();
    void reset();  // back to Idle once finished

    State state() const { return state_.load(); }
    float progress() const { return progress_.load(); }
    double elapsedSeconds() const;
    std::string log() const;

private:
    void run(std::vector<std::string> args, double total, std::string output);
    void handleLine(const std::string& line, double total);
    void appendLog(const std::string& line);

    std::thread thread_;
    std::atomic<State> state_{State::Idle};
    std::atomic<float> progress_{0.0f};
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> startMs_{0}, endMs_{0};
    mutable std::mutex logMutex_;
    std::string log_;
};
