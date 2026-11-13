#include "exporter.h"

#include "process.h"
#include "util.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

std::string num(double v) { return util::format("%.6f", v); }

// atempo only accepts factors in [0.5, 2.0] on older FFmpeg versions, so chain them.
std::string atempoChain(double speed) {
    if (std::fabs(speed - 1.0) < 1e-6) return std::string();
    std::string chain;
    auto add = [&](double v) {
        if (!chain.empty()) chain += ',';
        chain += "atempo=" + num(v);
    };
    double s = speed;
    while (s > 2.0 + 1e-9) {
        add(2.0);
        s /= 2.0;
    }
    while (s < 0.5 - 1e-9) {
        add(0.5);
        s /= 0.5;
    }
    if (std::fabs(s - 1.0) > 1e-6) add(s);
    return chain;
}

}  // namespace

double exportDuration(const std::vector<ExportClip>& clips) {
    double total = 0;
    for (const auto& c : clips) total += (c.out - c.in) / c.speed;
    return total;
}

std::vector<std::string> buildExportArgs(const std::string& ffmpeg, const std::vector<ExportClip>& clips,
                                         const ExportSettings& s) {
    std::vector<std::string> a = {ffmpeg, "-hide_banner", "-nostdin", "-y", "-v", "error", "-progress", "pipe:1", "-nostats"};

    const bool anyAudio = std::any_of(clips.begin(), clips.end(), [](const ExportClip& c) { return c.hasAudio; });

    // Input seeking (-ss/-t before -i) is fast and frame accurate when re-encoding.
    for (const auto& c : clips) a.insert(a.end(), {"-ss", num(c.in), "-t", num(c.out - c.in), "-i", c.path});

    const std::string fps = util::format("%d/%d", s.fpsNum, s.fpsDen);
    std::string graph, concatInputs;
    for (size_t i = 0; i < clips.size(); ++i) {
        const ExportClip& c = clips[i];
        const int idx = (int)i;
        const double outDur = (c.out - c.in) / c.speed;

        graph += util::format(
            "[%d:v:0]setpts=(PTS-STARTPTS)/%s,fps=%s,"
            "scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black,"
            "setsar=1,format=yuv420p[v%d];",
            idx, num(c.speed).c_str(), fps.c_str(), s.width, s.height, s.width, s.height, idx);
        concatInputs += util::format("[v%d]", idx);

        if (anyAudio) {
            if (c.hasAudio) {
                std::string tempo = atempoChain(c.speed);
                if (!tempo.empty()) tempo += ',';
                graph += util::format(
                    "[%d:a:0]asetpts=PTS-STARTPTS,%saresample=48000,"
                    "aformat=sample_fmts=fltp:channel_layouts=stereo,apad,atrim=duration=%s[a%d];",
                    idx, tempo.c_str(), num(outDur).c_str(), idx);
            } else {
                // Clips without audio get silence so every segment has the same streams.
                graph += util::format("anullsrc=r=48000:cl=stereo,atrim=duration=%s[a%d];", num(outDur).c_str(), idx);
            }
            concatInputs += util::format("[a%d]", idx);
        }
    }
    graph += concatInputs + util::format("concat=n=%d:v=1:a=%d[vout]%s", (int)clips.size(), anyAudio ? 1 : 0,
                                         anyAudio ? "[aout]" : "");

    a.insert(a.end(), {"-filter_complex", graph, "-map", "[vout]"});
    if (anyAudio) a.insert(a.end(), {"-map", "[aout]"});

    const int q = std::clamp(s.quality, 0, 2);
    if (s.encoder == "libx264") {
        static const char* crf[] = {"18", "23", "28"};
        a.insert(a.end(), {"-c:v", "libx264", "-preset", "veryfast", "-crf", crf[q]});
    } else if (s.encoder == "mpeg4") {
        static const char* qv[] = {"2", "4", "7"};
        a.insert(a.end(), {"-c:v", "mpeg4", "-q:v", qv[q]});
    } else {
        static const double bitsPerPixel[] = {0.15, 0.09, 0.05};
        double bps = (double)s.width * s.height * ((double)s.fpsNum / std::max(1, s.fpsDen)) * bitsPerPixel[q];
        a.insert(a.end(), {"-c:v", s.encoder, "-b:v", std::to_string((long long)bps)});
    }
    a.insert(a.end(), {"-pix_fmt", "yuv420p"});
    if (anyAudio) {
        static const char* ab[] = {"192k", "160k", "128k"};
        a.insert(a.end(), {"-c:a", "aac", "-b:a", ab[q]});
    }
    const std::string ext = util::extensionLower(s.output);
    if (ext == "mp4" || ext == "mov" || ext == "m4v") a.insert(a.end(), {"-movflags", "+faststart"});
    a.push_back(s.output);
    return a;
}

ExportJob::~ExportJob() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

void ExportJob::start(std::vector<std::string> args, double totalSeconds, std::string outputPath) {
    if (state_ == State::Running) return;
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        log_.clear();
    }
    cancel_ = false;
    progress_ = 0.0f;
    startMs_ = SDL_GetTicks();
    endMs_ = 0;
    state_ = State::Running;
    thread_ = std::thread(&ExportJob::run, this, std::move(args), totalSeconds, std::move(outputPath));
}

void ExportJob::cancel() { cancel_ = true; }

void ExportJob::reset() {
    if (state_ == State::Running) return;
    if (thread_.joinable()) thread_.join();
    state_ = State::Idle;
    progress_ = 0.0f;
}

double ExportJob::elapsedSeconds() const {
    uint64_t s = startMs_, e = endMs_;
    if (!s) return 0;
    if (!e) e = SDL_GetTicks();
    return (double)(e - s) / 1000.0;
}

std::string ExportJob::log() const {
    std::lock_guard<std::mutex> lock(logMutex_);
    return log_;
}

void ExportJob::appendLog(const std::string& line) {
    std::lock_guard<std::mutex> lock(logMutex_);
    log_ += line;
    log_ += '\n';
    if (log_.size() > 16 * 1024) {
        size_t cut = log_.find('\n', log_.size() - 12 * 1024);
        if (cut != std::string::npos) log_.erase(0, cut + 1);
    }
}

void ExportJob::handleLine(const std::string& line, double total) {
    if (line.empty()) return;
    size_t eq = line.find('=');
    bool keyValue = eq != std::string::npos && eq > 0 &&
                    std::all_of(line.begin(), line.begin() + (std::ptrdiff_t)eq, [](char c) {
                        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                    });
    if (!keyValue) {  // anything that is not -progress output is an error message
        appendLog(line);
        return;
    }
    const std::string key = line.substr(0, eq);
    if ((key == "out_time_us" || key == "out_time_ms") && total > 0) {
        long long us = std::atoll(line.c_str() + eq + 1);
        if (us > 0) {
            float p = std::min(0.999f, (float)((double)us / 1e6 / total));
            if (p > progress_) progress_ = p;
            wakeMainLoop();
        }
    }
}

void ExportJob::run(std::vector<std::string> args, double total, std::string output) {
    Process proc;
    std::string err;
    if (!proc.start(args, Process::Stderr::Merge, &err)) {
        appendLog("No se pudo iniciar FFmpeg: " + err);
        endMs_ = SDL_GetTicks();
        state_ = State::Failed;
        wakeMainLoop();
        return;
    }

    std::string pending;
    char buf[4096];
    bool cancelled = false;
    for (;;) {
        if (cancel_ && !cancelled) {
            proc.terminate();
            cancelled = true;
        }
        int n = proc.read(buf, (int)sizeof(buf), 100);
        if (n < 0) break;
        if (n == 0) continue;
        pending.append(buf, (size_t)n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            handleLine(line, total);
        }
    }
    if (!pending.empty()) handleLine(pending, total);

    const int code = proc.wait();
    endMs_ = SDL_GetTicks();
    if (cancelled) {
        util::removeFile(output);
        state_ = State::Cancelled;
    } else if (code == 0) {
        progress_ = 1.0f;
        state_ = State::Done;
    } else {
        util::removeFile(output);
        if (log().empty()) appendLog(util::format("FFmpeg terminó con el código %d", code));
        state_ = State::Failed;
    }
    wakeMainLoop();
}
