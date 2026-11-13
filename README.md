# NebulaTrim

A small, focused video clip editor for **trimming**, **changing speed** (a free slider from 0.1× to 10×) and **joining several clips** into a single video. Nothing more.

![NebulaTrim](docs/captura.png)

NebulaTrim is for quick edits: cut the boring parts, speed up or slow down a section, stitch a few clips together and export. It deliberately has no audio editing, effects or multi-track timeline.

- Written in C++17 with [SDL3](https://libsdl.org) and [Dear ImGui](https://github.com/ocornut/imgui) for the interface, [libmpv](https://mpv.io) for the preview, and the [FFmpeg](https://ffmpeg.org) command line for analysis and export.
- No Electron and no embedded browser: the executable is about **1 MB**.
- Runs on Linux and Windows.

## Why I made this

I just needed a lightweight video editor. I couldn't find one that fit what I wanted, so I built it myself, and I thought it might be useful to someone else too.

## Features

- **Add videos** by dragging them into the window, with the *Agregar videos* button, or from the command line (`nebulatrim a.mp4 b.mkv`).
- **Trim** by dragging the purple handles on the timeline, or place the playhead and press *Inicio aquí* (`I`) / *Fin aquí* (`O`). Frame stepping (`←` `→`) allows exact cuts.
- **Split** a clip in two at the playhead (`S`), useful for removing a section from the middle or speeding up just one part.
- **Per-clip speed** with a continuous logarithmic slider (1× in the center). `Ctrl+click` the slider to type an exact value. Audio keeps its pitch.
- **Reorder** clips by dragging their cards; right-click to duplicate, move or remove.
- **Play everything** to preview the final result before exporting.
- **Undo/redo** (`Ctrl+Z` / `Ctrl+Y`).
- **Export** to MP4 (H.264 + AAC, or MKV / MOV) with a choice of resolution, FPS and quality. Clips with a different resolution or in portrait are letterboxed, phone videos are rotated automatically, and clips without audio are padded with silence.

### Keyboard shortcuts

| Key | Action |
|---|---|
| `Space` / `K` | Play / pause |
| `←` / `→` | Previous / next frame |
| `Shift` + `←` / `→` | Back / forward 1 second |
| `I` / `O` | Set trim start / end |
| `S` | Split the clip at the playhead |
| `Home` / `End` | Jump to trim start / end |
| `↑` / `↓` | Previous / next clip |
| `Del` | Remove the selected clip |
| `Ctrl+D` | Duplicate the clip |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `Ctrl+O` | Add videos |
| `Ctrl+E` | Export |

With the mouse: click the video to play/pause; click or drag on the timeline to move the playhead.

## Requirements

NebulaTrim draws its interface and preview with **OpenGL 3.2 (core profile)**, so it needs a GPU and driver that support it. Any reasonably recent integrated graphics (Intel, AMD) works; a dedicated GPU is not required. Video decoding for the preview is done on the CPU by default, and export is handled by FFmpeg.

## Building on Linux

Dependencies: a C++17 compiler, CMake ≥ 3.16, SDL3, libmpv and FFmpeg (`ffmpeg` and `ffprobe`).

```bash
# Arch Linux
sudo pacman -S --needed base-devel cmake sdl3 mpv ffmpeg

# Debian 13+ / Ubuntu 25.04+
sudo apt install build-essential cmake pkg-config libsdl3-dev libmpv-dev ffmpeg

# Fedora (mpv and ffmpeg from RPM Fusion)
sudo dnf install gcc-c++ cmake SDL3-devel mpv-devel ffmpeg
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/nebulatrim                      # or: ./build/nebulatrim video1.mp4 video2.mp4
sudo cmake --install build              # optional: installs the binary and the .desktop launcher
```

The open/save dialogs use the desktop portal (`xdg-desktop-portal`) or `zenity`. Dragging files into the window always works.

## Building for Windows

> **Note:** the Windows build cross-compiles cleanly, but it has not yet been tested on a real Windows machine.

### From Linux (recommended, one command)

```bash
./scripts/build-windows.sh
```

The first run downloads into `.cache/windows/` (~210 MB) the [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) compiler and the Windows builds of SDL3, libmpv ([shinchiro](https://github.com/shinchiro/mpv-winbuild-cmake)) and FFmpeg ([BtbN](https://github.com/FFmpeg-Builds/FFmpeg-Builds)). It produces `dist/NebulaTrim-windows-x64.zip`: unzip it on Windows 10/11 and run `nebulatrim.exe`, with nothing else to install.

The unpacked package is about 300 MB, almost all of it codecs in `libmpv-2.dll` and `avcodec-*.dll`. The program itself is 1 MB.

### On Windows with MSYS2

From the **MSYS2 UCRT64** terminal:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,pkgconf,sdl3,mpv,ffmpeg}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# To use it outside MSYS2, bundle the .exe with its DLLs:
mkdir -p dist/NebulaTrim && cp build/nebulatrim.exe /ucrt64/bin/{ffmpeg,ffprobe}.exe dist/NebulaTrim/
for f in dist/NebulaTrim/*.exe; do ldd "$f" | awk '/ucrt64/ {print $3}' | xargs -r cp -n -t dist/NebulaTrim/; done
```

On Windows, NebulaTrim looks for `ffmpeg.exe` and `ffprobe.exe` next to `nebulatrim.exe` first, then in the `PATH`.

## Troubleshooting

| Environment variable | Purpose |
|---|---|
| `NEBULATRIM_FFMPEG`, `NEBULATRIM_FFPROBE` | Path to alternative FFmpeg executables. |
| `NEBULATRIM_MPV_OPTS` | Extra comma-separated mpv options for the preview, e.g. `hwdec=auto-safe,gpu-hwdec-interop=auto` to always decode on the GPU (less CPU, more RAM). |
| `NEBULATRIM_DEBUG=1` | Prints playback decisions to the terminal. |
| `NEBULATRIM_SNAPSHOT=capture.ppm` | (Development) Renders without showing the window, saves a frame and exits. |

If an export fails, the dialog shows FFmpeg's error with a button to copy it.

## Project layout

```
src/
  main.cpp       SDL3 + OpenGL window, event loop that sleeps when idle
  app.*          state (clips, undo/redo) and the whole UI
  player.*       libmpv preview rendered to a texture
  media.*        ffprobe (duration, size, rotation, FPS) and thumbnails
  exporter.*     builds the FFmpeg filter (trim, speed, scale, concat) and runs it
  process.*      child processes without a shell (Linux) or console (Windows)
  ui.*           dark theme, system font and vector icons
third_party/imgui  Dear ImGui 1.93 (core + SDL3/OpenGL3 backends only)
tests/export_test  smoke test: probe → thumbnails → export → cancel
```

Smoke test (requires `ffmpeg`):

```bash
cmake -S . -B build -DNEBULATRIM_TESTS=ON && cmake --build build -j
./build/export_test /tmp/output.mp4 video1.mp4 video2.mkv
```

## License

NebulaTrim is free software released under the **GNU General Public License v3.0** (see [`LICENSE`](LICENSE)).

It links against libmpv, which is GPL (v2 or later), and it runs FFmpeg as a separate process. Both are compatible with GPLv3. Third-party components:

| Component | License |
|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT (`third_party/imgui/LICENSE.txt`) |
| [SDL3](https://libsdl.org) | zlib |
| [libmpv](https://mpv.io) | GPLv2+ |
| [FFmpeg](https://ffmpeg.org) | LGPL/GPL, depending on the build (the Windows package uses a GPL build) |

The Windows package produced by `build-windows.sh` includes `LICENSE`, `LICENSE-imgui.txt`, `LICENSE-ffmpeg.txt` and `THIRD-PARTY-NOTICES.txt`. If you redistribute it, keep those files and make the corresponding source available.
