/***

    Olive - Non-Linear Video Editor
    Copyright (C) 2019  Olive Team

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#ifndef EXPORTTHREAD_H
#define EXPORTTHREAD_H

#include <atomic>
#include <vector>
#include <QThread>
#include <QMutex>
#include <QOffscreenSurface>
#include <QWaitCondition>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct AVCodec;
struct SwsContext;
struct SwrContext;
class RenderThread;

// One horizontal band of a 10-bit frame, converted on its own thread. in0/in1 include the overlap rows that keep
// the chroma filter at the band edges identical to converting the whole frame; y0/y1 are the rows kept.
struct ExportSwsBand {
  int y0{0}, y1{0}, in0{0}, in1{0};
  SwsContext* ctx{nullptr};
  AVFrame* tmp{nullptr};
};

extern "C" {
#include <libavcodec/avcodec.h>
}

#define COMPRESSION_TYPE_CBR 0
#define COMPRESSION_TYPE_CFR 1
#define COMPRESSION_TYPE_TARGETSIZE 2
#define COMPRESSION_TYPE_TARGETBR 3
// Variable bitrate: the bitrate field is the average target and peaks reach VBR_PEAK_RATIO times that value
#define COMPRESSION_TYPE_VBR 4
#define VBR_PEAK_RATIO 1.5

// structs that store parameters passed from the export dialogs to this thread

struct ExportParams {
  // export parameters
  QString filename;
  bool video_enabled;
  int video_codec;
  // Encoder name when a specific one is chosen (e.g. "h264_nvenc"); empty uses the codec's default encoder
  QString video_encoder;
  int video_width;
  int video_height;
  double video_frame_rate;
  int video_compression_type;
  double video_bitrate;
  // 10-bit output: the encoder's 10-bit pixel format is used instead of the one chosen in Advanced settings
  bool video_10bit{false};
  bool audio_enabled;
  int audio_codec;
  int audio_sampling_rate;
  int audio_bitrate;
  long start_frame;
  long end_frame;
};

struct VideoCodecParams {
  int pix_fmt;
  int threads;
};

// The first 10-bit pixel format (9 to 12 bits per component) the encoder accepts, or AV_PIX_FMT_NONE if it has none
AVPixelFormat FirstTenBitPixFmt(const AVCodec* codec);

// Time spent on each video step, in microseconds, and the number of frames it covers
struct ExportPhaseTimes {
  qint64 wait_us = 0;     // waiting for the GPU to finish the next frame
  qint64 convert_us = 0;  // 10-bit half-float to 16-bit conversion on the CPU
  qint64 scale_us = 0;    // swscale to the encoder's pixel format
  qint64 encode_us = 0;   // encoder call, including the VAAPI upload when used
  std::vector<qint64> convert_thread_us;  // time each conversion thread spent in the window
  int frames = 0;
  bool logged_setup = false;  // the frame size and thread count are logged once per export

  void add(const ExportPhaseTimes& other) {
    wait_us += other.wait_us;
    convert_us += other.convert_us;
    scale_us += other.scale_us;
    encode_us += other.encode_us;
    frames += other.frames;
  }
};

class ExportThread : public QThread {
  Q_OBJECT
public:
  ExportThread(const ExportParams& params, const VideoCodecParams& vparams, QObject* parent = nullptr);
  void run() override;

  // Pre-created GL fallback surface (created on GUI thread, passed through to
  // the internal RenderThread).  ExportThread does NOT take ownership.
  void setGlFallbackSurface(QOffscreenSurface* surface);

  const QString& GetError();

  // Text for the export summary dialog, set when the export completes successfully (empty otherwise)
  const QString& GetSummary() const;

  bool WasInterrupted();
signals:
  void ProgressChanged(int value, qint64 remaining_ms);
public slots:
  void Interrupt();

  void play_wake();
private:
  bool Encode(AVFormatContext* ofmt_ctx, AVCodecContext* codec_ctx, AVFrame* frame, AVPacket* packet, AVStream* stream);
  bool SetupVideo();
  bool SetupAudio();
  bool SetupContainer();
  bool Export();
  void Cleanup();

  std::atomic<bool> interrupt_;

  // params imported from dialogs
  ExportParams params_;
  VideoCodecParams vcodec_params_;

  AVFormatContext* fmt_ctx{nullptr};
  AVStream* video_stream{nullptr};
  const AVCodec* vcodec{nullptr};
  AVCodecContext* vcodec_ctx{nullptr};
  AVFrame* video_frame{nullptr};
  // Second frame buffer: the next frame renders into it while the current one is encoded
  AVFrame* video_frame_alt{nullptr};
  AVFrame* sws_frame{nullptr};
  SwsContext* sws_ctx{nullptr};
  // Non-empty when 10-bit output is converted in bands on several threads; otherwise sws_ctx is used
  std::vector<ExportSwsBand> sws_bands_;
  // Software format frames are converted to before encoding; VAAPI encoders take it from GPU surfaces instead
  AVPixelFormat sw_pix_fmt{AV_PIX_FMT_NONE};
  // True when the chosen output format is 10-bit or deeper: frames are rendered at 16 bits per channel
  bool video_10bit_{false};
  // The GPU writes the 10-bit 4:2:0 planes and the frame is encoded as rendered (no swscale)
  bool yuv_direct_{false};
  QString summary_;
  qint64 export_start_ms_{0};
  QString BuildSummary(const ExportPhaseTimes& totals, qint64 elapsed_ms) const;
  AVBufferRef* hw_device_ctx{nullptr};
  AVBufferRef* hw_frames_ctx{nullptr};
  AVStream* audio_stream{nullptr};
  const AVCodec* acodec{nullptr};
  AVFrame* audio_frame{nullptr};
  AVFrame* swr_frame{nullptr};
  AVCodecContext* acodec_ctx{nullptr};
  AVPacket* video_pkt{nullptr};
  AVPacket* audio_pkt{nullptr};
  SwrContext* swr_ctx{nullptr};

  int aframe_bytes;
  int ret;
  char* c_filename{nullptr};

  QMutex mutex;
  QWaitCondition waitCond;
  bool render_complete_{false};
  bool container_opened_{false};  // avio_open succeeded: a file exists at params_.filename

  QString export_error;

  std::atomic<bool> waiting_for_audio_;
  QOffscreenSurface* gl_fallback_surface_{nullptr};
private slots:
  void wake();

  // Pipelined rendering helpers. wait_for_render() returns 0 when the frame is ready, 1 when interrupted and 2
  // when the GPU renderer failed. Callers hold mutex, as run() does.
  void start_video_render(RenderThread* renderer, AVFrame* buf);
  int wait_for_render(RenderThread* renderer);
  int render_frame_sync(RenderThread* renderer, AVFrame* buf);
};

#endif // EXPORTTHREAD_H
