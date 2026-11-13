// Smoke test for the probe/thumbnail/export pipeline (no UI).
// Usage: export_test <output.mp4> <video1> [video2 ...]
// Each clip is trimmed to 20%-80% of its length and given a different speed; the result's
// duration must match the expected one. A second run checks that cancelling removes the file.
#include "exporter.h"
#include "media.h"
#include "util.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

void wakeMainLoop() {}

static ExportJob::State waitJob(ExportJob& job, int cancelAfterMs) {
    const uint64_t start = SDL_GetTicks();
    while (job.state() == ExportJob::State::Running) {
        if (cancelAfterMs >= 0 && SDL_GetTicks() - start > (uint64_t)cancelAfterMs) job.cancel();
        SDL_Delay(50);
    }
    return job.state();
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "uso: %s <salida.mp4> <video1> [video2 ...]\n", argv[0]);
        return 2;
    }
    SDL_Init(0);
    media::initTools();
    std::string err;
    const std::string encoder = media::detectVideoEncoder(&err);
    if (encoder.empty()) {
        std::fprintf(stderr, "FFmpeg no disponible: %s\n", err.c_str());
        return 1;
    }
    std::printf("encoder: %s\n", encoder.c_str());

    static const double kSpeeds[] = {1.5, 0.5, 1.0, 2.0, 3.7, 0.3};
    std::vector<ExportClip> clips;
    MediaInfo first;
    for (int i = 2; i < argc; ++i) {
        const MediaInfo mi = media::probe(argv[i]);
        if (!mi.ok) {
            std::fprintf(stderr, "probe falló para %s: %s\n", argv[i], mi.error.c_str());
            return 1;
        }
        if (clips.empty()) first = mi;
        ExportClip c;
        c.path = argv[i];
        c.in = mi.duration * 0.2;
        c.out = mi.duration * 0.8;
        c.speed = kSpeeds[(i - 2) % 6];
        c.hasAudio = mi.hasAudio;
        clips.push_back(c);

        std::vector<unsigned char> rgba;
        const bool thumbOk = media::thumbnail(c.path, c.in, 256, 144, rgba);
        std::printf("%-40s %7.3fs %4dx%-4d %d/%d fps audio=%d thumb=%s speed=%.2f\n", util::fileName(c.path).c_str(),
                    mi.duration, mi.width, mi.height, mi.fpsNum, mi.fpsDen, (int)mi.hasAudio, thumbOk ? "ok" : "FALLO",
                    c.speed);
        if (!thumbOk) return 1;
    }

    ExportSettings s;
    s.output = argv[1];
    s.width = std::max(2, first.width / 2 * 2);
    s.height = std::max(2, first.height / 2 * 2);
    s.fpsNum = first.fpsNum;
    s.fpsDen = first.fpsDen;
    s.encoder = encoder;
    const double expected = exportDuration(clips);
    const std::vector<std::string> args = buildExportArgs(media::ffmpegPath(), clips, s);

    ExportJob job;
    job.start(args, expected, s.output);
    if (waitJob(job, -1) != ExportJob::State::Done) {
        std::fprintf(stderr, "la exportación falló:\n%s\n", job.log().c_str());
        return 1;
    }
    const MediaInfo out = media::probe(s.output);
    std::printf("exportado en %.2fs: esperado %.3fs, obtenido %.3fs (%dx%d, audio=%d)\n", job.elapsedSeconds(), expected,
                out.duration, out.width, out.height, (int)out.hasAudio);
    if (!out.ok || std::fabs(out.duration - expected) > 0.15) return 1;

    // Cancelling must stop ffmpeg and remove the partial file.
    const std::string partial = s.output + ".cancel.mp4";
    ExportSettings s2 = s;
    s2.output = partial;
    job.reset();
    job.start(buildExportArgs(media::ffmpegPath(), clips, s2), expected, partial);
    const ExportJob::State st = waitJob(job, 150);
    std::printf("cancelación: estado=%d, archivo parcial %s\n", (int)st, util::pathExists(partial) ? "EXISTE" : "eliminado");
    if (st != ExportJob::State::Cancelled && st != ExportJob::State::Done) return 1;
    if (st == ExportJob::State::Cancelled && util::pathExists(partial)) return 1;
    util::removeFile(partial);

    std::printf("OK\n");
    SDL_Quit();
    return 0;
}
