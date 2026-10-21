FROM fedora:42 AS build

# Cross-compilation toolchain + Qt6 mingw packages + native Qt6 tools (lrelease, moc, etc.)
RUN dnf install -y \
    mingw64-gcc-c++ \
    mingw64-qt6-qtbase \
    mingw64-qt6-qtsvg \
    mingw64-qt6-qttools \
    mingw64-qt6-qtmultimedia \
    mingw64-qt6-qtshadertools \
    qt6-qtbase-devel \
    qt6-qttools-devel \
    qt6-qtshadertools-devel \
    cmake \
    make \
    && dnf clean all

# Download pre-built FFmpeg 8.1 shared libs (MinGW, GPL, includes all common codecs)
# BtbN only keeps the two most recent majors in the 'latest' release: n7.1 was pulled and
# the download 404s. Keep this inside the 3.4-8 range the compat guards cover.
ARG FFMPEG_ARCHIVE=ffmpeg-n8.1-latest-win64-gpl-shared-8.1
RUN dnf install -y unzip curl && \
    curl -fL -o /tmp/ffmpeg.zip \
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/${FFMPEG_ARCHIVE}.zip" && \
    unzip /tmp/ffmpeg.zip -d /tmp && \
    SYSROOT=/usr/x86_64-w64-mingw32/sys-root/mingw && \
    cp -r /tmp/${FFMPEG_ARCHIVE}/include/* "${SYSROOT}/include/" && \
    cp /tmp/${FFMPEG_ARCHIVE}/lib/*.dll.a "${SYSROOT}/lib/" && \
    mkdir -p "${SYSROOT}/lib/pkgconfig" && \
    cp /tmp/${FFMPEG_ARCHIVE}/lib/pkgconfig/*.pc "${SYSROOT}/lib/pkgconfig/" && \
    cp /tmp/${FFMPEG_ARCHIVE}/bin/*.dll "${SYSROOT}/bin/" && \
    rm -rf /tmp/ffmpeg*

ARG GIT_HASH
ARG VERSION=dev

COPY src/ /src
COPY packaging/ /packaging
COPY LICENSE /LICENSE
WORKDIR /src/build

RUN dnf install -y git autoconf libtool wget automake && dnf clean all
RUN git clone https://github.com/xiph/rnnoise /tmp/rnnoise && \
    cd /tmp/rnnoise && \
    ./autogen.sh && \
    mingw64-configure --prefix=/usr/x86_64-w64-mingw32/sys-root/mingw && \
    make && \
    make install && \
    rm -rf /tmp/rnnoise

RUN mingw64-cmake -DCMAKE_BUILD_TYPE=Release \
      ${GIT_HASH:+-DGIT_HASH=${GIT_HASH}} \
      -DCITRINE_VERSION=${VERSION} \
      -DQT_HOST_PATH=/usr \
      -DQT_HOST_PATH_CMAKE_DIR=/usr/lib64/cmake .. && \
    make -j$(nproc)

# --- Packaging stage ---
FROM build AS package

RUN dnf install -y curl unzip zip mingw64-nsis mingw-nsis-base && dnf clean all

COPY packaging/ /packaging/

# Build Frei0r plugins for Windows from source. The pre-built v2.5.4 win64 zip is linked against the debug C runtime
# (ucrtbased.dll, VCRUNTIME140_1D.dll), so most of its plugins fail to load on a normal Windows PC.
RUN curl -sL -o /tmp/frei0r.tar.gz "https://github.com/dyne/frei0r/archive/refs/tags/v2.5.4.tar.gz" && \
    mkdir -p /tmp/frei0r-src /tmp/frei0r-build && \
    tar -xzf /tmp/frei0r.tar.gz -C /tmp/frei0r-src --strip-components=1 && \
    rm /tmp/frei0r.tar.gz && \
    cd /tmp/frei0r-build && \
    mingw64-cmake -DCMAKE_BUILD_TYPE=Release -DWITHOUT_OPENCV=ON -DWITHOUT_FACERECOGNITION=ON \
      -DWITHOUT_CAIRO=ON -DWITHOUT_GAVL=ON /tmp/frei0r-src && \
    make -j$(nproc)

# Collect everything into /out
RUN SYSROOT=/usr/x86_64-w64-mingw32/sys-root/mingw && \
    mkdir -p /out/platforms /out/imageformats /out/multimedia /out/effects /out/ts && \
    cp citrine-editor.exe /out/ && \
    cp "${SYSROOT}/bin/"*.dll /out/ && \
    cp "${SYSROOT}/lib/qt6/plugins/platforms/qwindows.dll" /out/platforms/ && \
    cp "${SYSROOT}/lib/qt6/plugins/imageformats/"*.dll /out/imageformats/ 2>/dev/null || true && \
    cp "${SYSROOT}/lib/qt6/plugins/multimedia/"*.dll /out/multimedia/ 2>/dev/null || true && \
    cp /src/effects/shaders/*.xml /src/effects/shaders/*.frag /src/effects/shaders/*.vert /out/effects/ 2>/dev/null || true && \
    mkdir -p /out/effects/frei0r-1 && \
    find /tmp/frei0r-build -name '*.dll' -exec cp -n -t /out/effects/frei0r-1/ {} + && \
    cp /src/build/*.qm /out/ts/ 2>/dev/null || true && \
    mkdir -p /out/exportpresets && \
    cp /src/exportpresets/* /out/exportpresets/ 2>/dev/null || true

# makensis hardcodes x86-unicode but mingw64-nsis ships amd64-unicode
RUN cd /usr/share/nsis/Stubs && \
    for f in *-amd64-unicode; do ln -sf "$f" "${f/amd64/x86}"; done && \
    ln -sf amd64-unicode /usr/share/nsis/Plugins/x86-unicode

# Build NSIS installer
RUN cd /packaging/windows/nsis && \
    cp -r /out citrine && \
    cp /LICENSE . && \
    makensis -DX64 citrine.nsi && \
    cp *.exe /out/citrine-setup.exe

# Build portable zip (same content as NSIS + LICENSE)
RUN cd /packaging/windows/nsis && \
    cp /LICENSE citrine/ && \
    zip -r /out/citrine-portable.zip citrine/

# --- Output stage (for --output) ---
FROM scratch AS installer
COPY --from=package /out/citrine-setup.exe /citrine-setup.exe
COPY --from=package /out/citrine-portable.zip /citrine-portable.zip
