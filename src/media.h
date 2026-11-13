// Media helpers built on the ffprobe / ffmpeg command line tools.
#pragma once

#include <string>
#include <vector>

struct MediaInfo {
    bool ok = false;
    std::string error;
    double duration = 0;        // seconds
    int width = 0, height = 0;  // display size (sample aspect ratio and rotation applied)
    int fpsNum = 30, fpsDen = 1;
    bool hasAudio = false;

    double fps() const { return fpsDen > 0 && fpsNum > 0 ? (double)fpsNum / fpsDen : 30.0; }
};

namespace media {

// Looks for ffmpeg/ffprobe next to the executable first, then in PATH.
// Overridable with NEBULATRIM_FFMPEG / NEBULATRIM_FFPROBE.
void initTools();
const std::string& ffmpegPath();
const std::string& ffprobePath();

MediaInfo probe(const std::string& path);

// Decodes one frame at time `t` scaled/letterboxed into exactly w x h RGBA pixels.
bool thumbnail(const std::string& path, double t, int w, int h, std::vector<unsigned char>& rgba);

// Checks ffmpeg/ffprobe and returns the H.264 (or fallback) encoder to use.
// Returns an empty string and fills `error` if the tools are not usable.
std::string detectVideoEncoder(std::string* error);

}  // namespace media
