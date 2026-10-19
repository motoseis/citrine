# Ubuntu and Kubuntu Prerequisites for Citrine Video Editor

These steps were tested on Kubuntu 26.04, which ships Qt 6.12. Ubuntu 24.04 ships an older Qt, so the Qt packages may not provide everything Citrine needs there. The AppImage in the README bundles its own Qt and is the simpler option if the source build fails.

Install the build tools and development packages:

```bash
sudo apt update
sudo apt install build-essential cmake
sudo apt install qt6-base-dev qt6-base-private-dev libqt6opengl6-dev
sudo apt install qt6-multimedia-dev
sudo apt install libqt6shadertools6 qt6-shadertools-dev
sudo apt install libqt6svg6 qt6-svg-dev
sudo apt install qt6-tools-dev
sudo apt install ffmpeg
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev libavdevice-dev libswscale-dev libswresample-dev
```

Then build as described in the [README](../README.md):

```bash
cmake -S src -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```
