#include "media.h"

#include "process.h"
#include "util.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace media {
namespace {

std::string g_ffmpeg = "ffmpeg";
std::string g_ffprobe = "ffprobe";

std::string findTool(const char* envVar, const char* name) {
    if (const char* env = SDL_getenv(envVar); env && *env) return env;
    std::string dir = util::executableDir();
#ifdef _WIN32
    std::string local = dir + name + ".exe";
#else
    std::string local = dir + name;
#endif
    SDL_PathInfo info;
    if (!dir.empty() && SDL_GetPathInfo(local.c_str(), &info) && info.type == SDL_PATHTYPE_FILE) return local;
    return name;  // resolved through PATH
}

bool parseRational(const std::string& s, double& out) {
    size_t sep = s.find_first_of("/:");
    if (sep == std::string::npos) {
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || !(v > 0)) return false;
        out = v;
        return true;
    }
    double num = std::atof(s.substr(0, sep).c_str());
    double den = std::atof(s.substr(sep + 1).c_str());
    if (!(num > 0) || !(den > 0)) return false;
    out = num / den;
    return true;
}

// Snaps odd/variable frame rates (phones, screen recorders) to a sane value.
void normalizeFps(double fps, int& num, int& den) {
    static const int common[][2] = {{24000, 1001}, {24, 1}, {25, 1},    {30000, 1001}, {30, 1},
                                    {48, 1},       {50, 1}, {60000, 1001}, {60, 1},    {90, 1},
                                    {100, 1},      {120, 1}, {144, 1},  {240, 1}};
    num = 30;
    den = 1;
    if (!(fps > 0) || !std::isfinite(fps)) return;
    double bestDiff = 1e9;
    for (const auto& c : common) {
        double v = (double)c[0] / c[1];
        double diff = std::fabs(fps - v) / v;
        if (diff < bestDiff) {
            bestDiff = diff;
            num = c[0];
            den = c[1];
        }
    }
    if (bestDiff > 0.03) {
        num = std::clamp((int)std::lround(fps), 1, 240);
        den = 1;
    }
}

struct StreamEntry {
    std::string type;
    int width = 0, height = 0;
    std::string sar, avgRate, rawRate;
    bool attachedPic = false;
    double rotation = 0;
};

}  // namespace

void initTools() {
    g_ffmpeg = findTool("NEBULATRIM_FFMPEG", "ffmpeg");
    g_ffprobe = findTool("NEBULATRIM_FFPROBE", "ffprobe");
}

const std::string& ffmpegPath() { return g_ffmpeg; }
const std::string& ffprobePath() { return g_ffprobe; }

MediaInfo probe(const std::string& path) {
    MediaInfo info;
    std::vector<std::string> args = {
        g_ffprobe, "-v", "error", "-show_entries",
        "format=duration:stream=codec_type,width,height,avg_frame_rate,r_frame_rate,sample_aspect_ratio"
        ":stream_disposition=attached_pic:stream_side_data=rotation:stream_tags=rotate",
        "-of", "default", path};
    std::string out;
    int code = runCapture(args, Process::Stderr::Merge, out, 4 * 1024 * 1024);
    if (code == -1 && out.find(g_ffprobe) == 0) {
        info.error = "No se pudo ejecutar ffprobe. ¿Está FFmpeg instalado?";
        return info;
    }

    std::vector<StreamEntry> streams;
    double duration = -1;
    enum { None, Stream, Format } section = None;
    std::string firstError;

    size_t start = 0;
    while (start <= out.size()) {
        size_t end = out.find('\n', start);
        if (end == std::string::npos) end = out.size();
        std::string line = out.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line == "[STREAM]") {
            streams.emplace_back();
            section = Stream;
            continue;
        }
        if (line == "[FORMAT]") {
            section = Format;
            continue;
        }
        if (line == "[/STREAM]" || line == "[/FORMAT]") {
            section = None;
            continue;
        }
        if (line[0] == '[') continue;  // nested sections such as [SIDE_DATA]

        size_t eq = line.find('=');
        if (eq == std::string::npos || section == None) {
            if (firstError.empty()) firstError = line;
            continue;
        }
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (section == Format) {
            if (key == "duration") duration = std::atof(val.c_str());
        } else if (!streams.empty()) {
            StreamEntry& s = streams.back();
            if (key == "codec_type") s.type = val;
            else if (key == "width") s.width = std::atoi(val.c_str());
            else if (key == "height") s.height = std::atoi(val.c_str());
            else if (key == "sample_aspect_ratio") s.sar = val;
            else if (key == "avg_frame_rate") s.avgRate = val;
            else if (key == "r_frame_rate") s.rawRate = val;
            else if (key == "DISPOSITION:attached_pic") s.attachedPic = (val == "1");
            else if (key == "rotation" || key == "TAG:rotate") s.rotation = std::atof(val.c_str());
        }
    }

    if (code != 0) {
        info.error = firstError.empty() ? "ffprobe no pudo leer el archivo" : firstError;
        return info;
    }

    const StreamEntry* video = nullptr;
    for (const auto& s : streams) {
        if (s.type == "video" && !s.attachedPic && s.width > 0 && s.height > 0) {
            if (!video) video = &s;
        } else if (s.type == "audio") {
            info.hasAudio = true;
        }
    }
    if (!video) {
        info.error = "El archivo no contiene video";
        return info;
    }
    if (!(duration > 0.05) || !std::isfinite(duration)) {
        info.error = "No se pudo determinar la duración (¿es una imagen?)";
        return info;
    }

    double w = video->width, h = video->height;
    double sar = 1;
    if (parseRational(video->sar, sar) && sar > 0 && std::fabs(sar - 1) > 1e-3) w *= sar;
    double rot = std::fmod(std::fabs(video->rotation), 180.0);
    if (std::fabs(rot - 90.0) < 1.0) std::swap(w, h);
    info.width = std::max(2, (int)std::lround(w));
    info.height = std::max(2, (int)std::lround(h));

    double fps = 0;
    if (!parseRational(video->avgRate, fps) || fps > 1000) parseRational(video->rawRate, fps);
    normalizeFps(fps, info.fpsNum, info.fpsDen);

    info.duration = duration;
    info.ok = true;
    return info;
}

bool thumbnail(const std::string& path, double t, int w, int h, std::vector<unsigned char>& rgba) {
    const size_t bytes = (size_t)w * (size_t)h * 4;
    std::string vf = util::format(
        "scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x0b0b0e", w, h, w, h);
    std::vector<std::string> args = {g_ffmpeg, "-v",     "quiet",    "-nostdin", "-ss", util::format("%.3f", std::max(0.0, t)),
                                     "-i",     path,     "-map",     "0:v:0",    "-frames:v", "1",
                                     "-vf",    vf,       "-f",       "rawvideo", "-pix_fmt",  "rgba", "pipe:1"};
    std::string out;
    int code = runCapture(args, Process::Stderr::Discard, out, bytes + 4096);
    if (code != 0 || out.size() < bytes) return false;
    rgba.assign(out.begin(), out.begin() + (std::ptrdiff_t)bytes);
    return true;
}

std::string detectVideoEncoder(std::string* error) {
    std::string out;
    if (runCapture({g_ffprobe, "-version"}, Process::Stderr::Merge, out) != 0) {
        if (error) *error = "No se encontró ffprobe";
        return std::string();
    }
    if (runCapture({g_ffmpeg, "-hide_banner", "-encoders"}, Process::Stderr::Merge, out) != 0) {
        if (error) *error = "No se encontró ffmpeg";
        return std::string();
    }
    for (const char* enc : {"libx264", "libopenh264", "h264_mf", "mpeg4"}) {
        if (out.find(std::string(" ") + enc + " ") != std::string::npos) return enc;
    }
    if (error) *error = "Tu FFmpeg no tiene ningún codificador de video compatible";
    return std::string();
}

}  // namespace media
