# Citrine Video Editor

Citrine is a free, open-source non-linear video editor. It is a fork of [Amber Video Editor](https://github.com/baptisterajaut/amber), which continues [Olive 0.1](https://github.com/olive-editor/olive).

## Features

- **Multi-track timeline** with clip splitting, ripple/rolling/slip/slide tools, transitions, and keyframe animation
- **GPU-accelerated rendering** via Qt RHI (Vulkan, Metal, D3D12, OpenGL fallback)
- **Hardware video decoding** (VAAPI, D3D11VA, VideoToolbox), with automatic software fallback
- **Proxy media** generated automatically for imported and opened footage
- **Frei0r plugin support**

## Minimum requirements

- **GPU:** OpenGL 3.2+. Vulkan/Metal/D3D12 used when available.
- **CPU:** any x86-64 processor.
- **RAM:** 512 MB free for simple edits; more helps with long timelines and high-res footage.
- **Linux:** Arch natively (PKGBUILD); Ubuntu and Kubuntu from source (see [documentation/prerequisites-ubuntu.md](documentation/prerequisites-ubuntu.md)); other distributions via AppImage (bundles Qt 6.10).
- **Windows:** 10 or newer. **macOS:** Apple Silicon; Intel Macs untested.

## Build

```bash
# Linux
cmake -S src -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# macOS
brew install qt@6 ffmpeg cmake
cmake -S src -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6);$(brew --prefix ffmpeg)"
cmake --build build -j$(sysctl -n hw.ncpu)

# Arch Linux
cd packaging/linux && makepkg -si

# Docker — AppImage (Qt 6.10, covers Ubuntu 24.04+)
docker buildx build -f packaging/linux/appimage.dockerfile --output type=local,dest=./out .

# Docker — Windows NSIS installer (cross-compiled from Fedora)
docker build -f packaging/windows/cross-compile.dockerfile --target package -t citrine-win64 .
docker run --rm citrine-win64 cat /out/amber-setup.exe > amber-setup.exe
```

**Dependencies:** Qt 6.10+ (Core, Gui, GuiPrivate, Widgets, Multimedia, OpenGL, OpenGLWidgets, ShaderTools, ShaderToolsPrivate, Svg, LinguistTools), FFmpeg 3.4–8 (avutil, avcodec, avformat, avfilter, swscale, swresample).

For detailed build instructions, see [documentation/prerequisites-fedora.md](documentation/prerequisites-fedora.md) for **Fedora Linux 44** and [documentation/prerequisites-ubuntu.md](documentation/prerequisites-ubuntu.md) for **Ubuntu and Kubuntu**.

## License

GPLv3. Based on [Amber Video Editor](https://github.com/baptisterajaut/amber), [olive-editor/olive](https://github.com/olive-editor/olive) by [MattKC](https://github.com/itsmattkc) and the Olive Team.
