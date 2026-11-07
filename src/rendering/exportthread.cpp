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

#include "exportthread.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFloat16>
#include <QPainter>
#include <QtMath>

#include <thread>
#include <utility>
#include <vector>

#include "global/config.h"
#include "global/global.h"
#include "engine/sequence.h"
#include "core/appcontext.h"
#include "rendering/renderthread.h"
#include "rendering/renderfunctions.h"
#include "rendering/audio.h"
#include "global/debug.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif

AVPixelFormat FirstTenBitPixFmt(const AVCodec* codec) {
  if (codec == nullptr) return AV_PIX_FMT_NONE;

  // Hardware H.264 encoders list 10-bit formats but refuse to open them (only the software encoder does 10-bit H.264)
  if (codec->id == AV_CODEC_ID_H264 && (strstr(codec->name, "_nvenc") != nullptr || strstr(codec->name, "_vaapi") != nullptr)) {
    return AV_PIX_FMT_NONE;
  }

  // VAAPI encoders list only the hardware surface format, so report the software format the export feeds them
  if (strstr(codec->name, "_vaapi") != nullptr) {
    return codec->id == AV_CODEC_ID_HEVC ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NONE;
  }

  // Exactly 10 bits is preferred; 9, 11 and 12 bits are used only when the encoder has nothing else
  AVPixelFormat fallback = AV_PIX_FMT_NONE;
  auto consider = [&fallback](AVPixelFormat fmt) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (desc == nullptr || desc->comp[0].depth <= 8 || desc->comp[0].depth > 12) return AV_PIX_FMT_NONE;
    if (desc->comp[0].depth == 10) return fmt;
    if (fallback == AV_PIX_FMT_NONE) fallback = fmt;
    return AV_PIX_FMT_NONE;
  };

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 0, 0)
  const enum AVPixelFormat* fmts = nullptr;
  int count = 0;
  if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void**)&fmts, &count) != 0 ||
      fmts == nullptr) {
    return AV_PIX_FMT_NONE;
  }
  for (int i = 0; i < count; i++) {
    AVPixelFormat ten = consider(fmts[i]);
    if (ten != AV_PIX_FMT_NONE) return ten;
  }
#else
  if (codec->pix_fmts == nullptr) return AV_PIX_FMT_NONE;
  for (int i = 0; codec->pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
    AVPixelFormat ten = consider(codec->pix_fmts[i]);
    if (ten != AV_PIX_FMT_NONE) return ten;
  }
#endif
  return fallback;
}

// Hash of a pixel index, uniform in [0, 1)
static float dither_noise(quint32 n) {
  n ^= n >> 16;
  n *= 0x7feb352dU;
  n ^= n >> 15;
  n *= 0x846ca68bU;
  n ^= n >> 16;
  return float(n & 0xFFFFFFU) / float(0x1000000U);
}

// The 16-bit render target reads back as half-float RGBA. Rewrite each channel in place as an unsigned
// 16-bit value (0-65535) so swscale can read the frame as RGBA64. Half an 8-bit step of noise is added first,
// so an 8-bit source's steps become a dithered transition in the 10-bit output instead of hard bands.
//
// Converting each half float in software is the slow part of this loop, so every 16-bit pattern is
// converted once into a table. The table holds the same float values, so the output is unchanged.
static const float* half_to_float_table() {
  static const std::vector<float> table = [] {
    std::vector<float> t(65536);
    for (quint32 bits = 0; bits < 65536; bits++) {
      qfloat16 half;
      quint16 b = quint16(bits);
      memcpy(&half, &b, sizeof(quint16));
      t[bits] = float(half);
    }
    return t;
  }();
  return table.data();
}

// Each row is independent, so rows are split across threads. The per-row work is the same either way.
static void convert_rows(AVFrame* frame, int y0, int y1) {
  const int channels = 4;
  const float* half_to_float = half_to_float_table();
  for (int y = y0; y < y1; y++) {
    quint16* row = reinterpret_cast<quint16*>(frame->data[0] + y * frame->linesize[0]);
    for (int i = 0; i < frame->width * channels; i++) {
      float noise = dither_noise(quint32(y) * quint32(frame->width * channels) + quint32(i)) - 0.5f;
      float value = qBound(0.0f, half_to_float[row[i]] + noise / 255.0f, 1.0f);
      row[i] = quint16(value * 65535.0f + 0.5f);
    }
  }
}

// Overlap rows on each side of a band. Chroma filtering reaches a couple of rows past a band edge, so this is more than enough.
static const int kSwsBandOverlap = 16;

// Converts src into dst one band per thread. Each band writes only its own rows, so the result matches a single
// full-frame conversion (checked in a benchmark with the same band layout).
static void scale_bands(std::vector<ExportSwsBand>& bands, AVFrame* src, AVFrame* dst) {
  std::vector<std::thread> workers;
  for (auto& b : bands) {
    workers.emplace_back([&b, src] {
      const uint8_t* sp[4] = {src->data[0] + b.in0 * src->linesize[0], nullptr, nullptr, nullptr};
      int sl[4] = {src->linesize[0], 0, 0, 0};
      sws_scale(b.ctx, sp, sl, 0, b.in1 - b.in0, b.tmp->data, b.tmp->linesize);
    });
  }
  for (auto& w : workers) w.join();

  const int row_bytes = dst->width * 2;  // 16-bit samples
  for (const auto& b : bands) {
    const int ly = b.y0 - b.in0;
    for (int y = 0; y < b.y1 - b.y0; y++) {
      memcpy(dst->data[0] + (b.y0 + y) * dst->linesize[0], b.tmp->data[0] + (ly + y) * b.tmp->linesize[0], row_bytes);
    }
    // Chroma is 4:2:0 interleaved: one chroma row per two luma rows
    for (int y = 0; y < (b.y1 - b.y0) / 2; y++) {
      memcpy(dst->data[1] + (b.y0 / 2 + y) * dst->linesize[1], b.tmp->data[1] + (ly / 2 + y) * b.tmp->linesize[1], row_bytes);
    }
  }
}

// Number of threads used for the 16-bit conversion
static int conversion_threads() {
  return qBound(1, static_cast<int>(std::thread::hardware_concurrency()), 8);
}

// If thread_us is given, it receives the microseconds each worker thread spent (index = thread number)
static void half_float_to_unorm16(AVFrame* frame, std::vector<qint64>* thread_us = nullptr) {
  const int threads = conversion_threads();
  if (thread_us != nullptr) thread_us->assign(threads, 0);
  if (threads == 1 || frame->height < threads * 4) {
    QElapsedTimer t;
    t.start();
    convert_rows(frame, 0, frame->height);
    if (thread_us != nullptr) (*thread_us)[0] = t.nsecsElapsed() / 1000;
    return;
  }
  std::vector<std::thread> workers;
  for (int k = 0; k < threads; k++) {
    int y0 = frame->height * k / threads;
    int y1 = frame->height * (k + 1) / threads;
    workers.emplace_back([frame, thread_us, k, y0, y1] {
      QElapsedTimer t;
      t.start();
      convert_rows(frame, y0, y1);
      if (thread_us != nullptr) (*thread_us)[k] = t.nsecsElapsed() / 1000;
    });
  }
  for (auto& w : workers) w.join();
}

ExportThread::ExportThread(const ExportParams &params,
                           const VideoCodecParams& vparams,
                           QObject *parent) :
  QThread(parent),
  params_(params),
  vcodec_params_(vparams),
  interrupt_(false)
{
}

void ExportThread::setGlFallbackSurface(QOffscreenSurface* surface) {
  gl_fallback_surface_ = surface;
}

bool ExportThread::Encode(AVFormatContext* ofmt_ctx, AVCodecContext* codec_ctx, AVFrame* frame, AVPacket* packet, AVStream* stream) {
  ret = avcodec_send_frame(codec_ctx, frame);
  if (ret < 0) {
    qCritical() << "Failed to send frame to encoder." << ret;
    export_error = tr("failed to send frame to encoder (%1)").arg(QString::number(ret));
    return false;
  }

  while (ret >= 0) {
    ret = avcodec_receive_packet(codec_ctx, packet);
    if (ret == AVERROR(EAGAIN)) {
      return true;
    } else if (ret < 0) {
      if (ret != AVERROR_EOF) {
        qCritical() << "Failed to receive packet from encoder." << ret;
        export_error = tr("failed to receive packet from encoder (%1)").arg(QString::number(ret));
      }
      return false;
    }

    packet->stream_index = stream->index;

    av_packet_rescale_ts(packet, codec_ctx->time_base, stream->time_base);

    ret = av_interleaved_write_frame(ofmt_ctx, packet);
    av_packet_unref(packet);
    if (ret < 0) {
      qCritical() << "Failed to write packet." << ret;
      return false;
    }
  }
  return true;
}

bool ExportThread::SetupVideo() {
  // if video is disabled, no setup necessary
  if (!params_.video_enabled) return true;

  // find video encoder
  if (params_.video_encoder.isEmpty()) {
    vcodec = avcodec_find_encoder(static_cast<enum AVCodecID>(params_.video_codec));
  } else {
    vcodec = avcodec_find_encoder_by_name(params_.video_encoder.toUtf8().constData());
  }
  if (!vcodec) {
    qCritical() << "Could not find video encoder";
    export_error = tr("could not video encoder for %1").arg(QString::number(params_.video_codec));
    return false;
  }

  // create video stream
  video_stream = avformat_new_stream(fmt_ctx, vcodec);
  if (!video_stream) {
    qCritical() << "Could not allocate video stream";
    export_error = tr("could not allocate video stream");
    return false;
  }
  video_stream->id = 0;

  // allocate context
  //	vcodec_ctx = video_stream->codec;
  vcodec_ctx = avcodec_alloc_context3(vcodec);
  if (!vcodec_ctx) {
    qCritical() << "Could not allocate video encoding context";
    export_error = tr("could not allocate video encoding context");
    return false;
  }

  // setup context
  vcodec_ctx->codec_id = static_cast<enum AVCodecID>(params_.video_codec);
  vcodec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
  vcodec_ctx->width = params_.video_width;
  vcodec_ctx->height = params_.video_height;
  vcodec_ctx->sample_aspect_ratio = {1, 1};
  bool vaapi = params_.video_encoder.endsWith("_vaapi");
  AVPixelFormat encoder_pix_fmt = static_cast<AVPixelFormat>(vcodec_params_.pix_fmt);
  if (params_.video_10bit) {
    // The 10-bit switch uses the encoder's own 10-bit format, in place of the one set in Advanced settings
    AVPixelFormat ten_bit = FirstTenBitPixFmt(vcodec);
    if (ten_bit != AV_PIX_FMT_NONE) {
      encoder_pix_fmt = ten_bit;
    }
  }

  // 10-bit (or deeper) output is rendered at 16 bits per channel, so it needs 16-bit render targets
  const AVPixFmtDescriptor* pix_desc = av_pix_fmt_desc_get(encoder_pix_fmt);
  video_10bit_ = pix_desc != nullptr && pix_desc->comp[0].depth > 8;
  if (video_10bit_ && !amber::HighPrecisionIntermediates.load()) {
    export_error = tr("10-bit export needs 16-bit render targets, which this graphics backend does not support");
    return false;
  }

  // VAAPI encoders take GPU surfaces (NV12, or P010 for 10-bit); frames are uploaded from a software frame
  sw_pix_fmt = vaapi ? (video_10bit_ ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12) : encoder_pix_fmt;
  vcodec_ctx->pix_fmt = vaapi ? AV_PIX_FMT_VAAPI : sw_pix_fmt;
  vcodec_ctx->framerate = av_d2q(params_.video_frame_rate, INT_MAX);
  if (params_.video_compression_type == COMPRESSION_TYPE_CBR ||
      params_.video_compression_type == COMPRESSION_TYPE_VBR) {
    vcodec_ctx->bit_rate = qRound(params_.video_bitrate * 1000000);
  }
  if (params_.video_compression_type == COMPRESSION_TYPE_VBR) {
    // Peaks are capped at VBR_PEAK_RATIO times the target; the buffer holds two seconds of target bitrate
    vcodec_ctx->rc_max_rate = qRound(params_.video_bitrate * 1000000 * VBR_PEAK_RATIO);
    vcodec_ctx->rc_buffer_size = qRound(params_.video_bitrate * 1000000 * 2);
  }
  vcodec_ctx->time_base = av_inv_q(vcodec_ctx->framerate);

  // Describe the colours we actually write. sws_getContext() below defaults to the BT.601
  // matrix and limited range for RGBA->YUV, and without these tags the file says nothing:
  // players then assume BT.709 for HD, decode with the wrong matrix and show reds/magentas
  // too bright and greens/cyans too dark compared to the editor (#74). Metadata only - not
  // a single encoded pixel changes.
  vcodec_ctx->colorspace = AVCOL_SPC_SMPTE170M;
  vcodec_ctx->color_range = AVCOL_RANGE_MPEG;
  vcodec_ctx->color_primaries = AVCOL_PRI_BT709;
  vcodec_ctx->color_trc = AVCOL_TRC_BT709;
  video_stream->time_base = vcodec_ctx->time_base;

  if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
    vcodec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  // Some codecs require special settings so we set that up here
  switch (vcodec_ctx->codec_id) {

  /// H.264 specific settings
  case AV_CODEC_ID_H264:
  case AV_CODEC_ID_H265:
    switch (params_.video_compression_type) {
    case COMPRESSION_TYPE_CFR:
      if (params_.video_encoder.endsWith("_nvenc")) {
        // NVENC has no CRF; constant quality is VBR with a cq target on the same 0-51 scale
        av_opt_set(vcodec_ctx->priv_data, "rc", "vbr", AV_OPT_SEARCH_CHILDREN);
        av_opt_set(vcodec_ctx->priv_data, "cq", QString::number(static_cast<int>(params_.video_bitrate)).toUtf8(), AV_OPT_SEARCH_CHILDREN);
        // ffmpeg's NVENC defaults set a 2 Mbps target, and without an explicit ceiling the VBR output stays near
        // 10 Mbps whatever the cq value. Clear the target and set a high ceiling so the quality value decides.
        vcodec_ctx->bit_rate = 0;
        vcodec_ctx->rc_max_rate = 100000000;
      } else if (vaapi) {
        // VAAPI has no CRF either; constant QP is the closest match on a 0-52 scale
        av_opt_set(vcodec_ctx->priv_data, "rc_mode", "CQP", AV_OPT_SEARCH_CHILDREN);
        av_opt_set(vcodec_ctx->priv_data, "qp", QString::number(static_cast<int>(params_.video_bitrate)).toUtf8(), AV_OPT_SEARCH_CHILDREN);
      } else {
        av_opt_set(vcodec_ctx->priv_data, "crf", QString::number(static_cast<int>(params_.video_bitrate)).toUtf8(), AV_OPT_SEARCH_CHILDREN);
      }
      break;
    case COMPRESSION_TYPE_VBR:
      // The target and peak ceiling are already set above; NVENC and VAAPI need their rate control switched to VBR
      if (params_.video_encoder.endsWith("_nvenc")) {
        av_opt_set(vcodec_ctx->priv_data, "rc", "vbr", AV_OPT_SEARCH_CHILDREN);
      } else if (vaapi) {
        av_opt_set(vcodec_ctx->priv_data, "rc_mode", "VBR", AV_OPT_SEARCH_CHILDREN);
      }
      break;
    }
    break;

  default:
    break;
  }

  // Set export to be multithreaded
  AVDictionary* opts = nullptr;
  if (vcodec_params_.threads == 0) {
    av_dict_set(&opts, "threads", "auto", 0);
  } else {
    av_dict_set(&opts, "threads", QString::number(vcodec_params_.threads).toUtf8(), 0);
  }

  // VAAPI encoders need a device and a surface pool to draw frames from
  if (vaapi) {
    // CITRINE_VAAPI_DEVICE picks the DRM render node (e.g. /dev/dri/renderD129); by default FFmpeg chooses
    const QByteArray vaapi_device = qgetenv("CITRINE_VAAPI_DEVICE");
    if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                               vaapi_device.isEmpty() ? nullptr : vaapi_device.constData(), nullptr, 0) < 0) {
      qCritical() << "Could not create VAAPI device for export";
      export_error = tr("could not create VAAPI device for export");
      av_dict_free(&opts);
      return false;
    }
    hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
    if (hw_frames_ctx == nullptr) {
      export_error = tr("could not allocate VAAPI frame context");
      av_dict_free(&opts);
      return false;
    }
    AVHWFramesContext* frames = reinterpret_cast<AVHWFramesContext*>(hw_frames_ctx->data);
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = sw_pix_fmt;
    frames->width = params_.video_width;
    frames->height = params_.video_height;
    frames->initial_pool_size = 20;
    ret = av_hwframe_ctx_init(hw_frames_ctx);
    if (ret < 0) {
      qCritical() << "Could not initialise VAAPI frame context." << ret;
      export_error = tr("could not initialise VAAPI frame context (%1)").arg(QString::number(ret));
      av_dict_free(&opts);
      return false;
    }
    vcodec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    vcodec_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
  }

  // Open video encoder
  ret = avcodec_open2(vcodec_ctx, vcodec, &opts);
  av_dict_free(&opts);
  if (ret < 0) {
    qCritical() << "Could not open output video encoder." << ret;
    export_error = tr("could not open output video encoder (%1)").arg(QString::number(ret));
    return false;
  }

  // Copy video encoder parameters to output stream
  ret = avcodec_parameters_from_context(video_stream->codecpar, vcodec_ctx);
  if (ret < 0) {
    qCritical() << "Could not copy video encoder parameters to output stream." << ret;
    export_error = tr("could not copy video encoder parameters to output stream (%1)").arg(QString::number(ret));
    return false;
  }

  // Create raw AVFrame that will contain the RGBA buffer straight from compositing
  if (amber::ActiveSequence == nullptr) {
    export_error = tr("no active sequence");
    return false;
  }
  video_frame = av_frame_alloc();
  video_frame->format = video_10bit_ ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;
  video_frame->width = amber::ActiveSequence->width;
  video_frame->height = amber::ActiveSequence->height;
  ret = av_frame_get_buffer(video_frame, 0);
  if (ret < 0) {
    qCritical() << "Could not allocate video frame buffer." << ret;
    export_error = tr("could not allocate video frame buffer (%1)").arg(QString::number(ret));
    return false;
  }

  video_frame_alt = av_frame_alloc();
  video_frame_alt->format = video_frame->format;
  video_frame_alt->width = video_frame->width;
  video_frame_alt->height = video_frame->height;
  ret = av_frame_get_buffer(video_frame_alt, 0);
  if (ret < 0) {
    qCritical() << "Could not allocate video frame buffer." << ret;
    export_error = tr("could not allocate video frame buffer (%1)").arg(QString::number(ret));
    return false;
  }

  video_pkt = av_packet_alloc();

  // Set up conversion context
  sws_ctx = sws_getContext(
        amber::ActiveSequence->width,
        amber::ActiveSequence->height,
        video_10bit_ ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA,
        params_.video_width,
        params_.video_height,
        sw_pix_fmt,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr
        );

  // 10-bit output from a sequence at the export size is converted in bands on several threads
  const int height = params_.video_height;
  const int threads = qBound(1, static_cast<int>(std::thread::hardware_concurrency()), 8);
  if (video_10bit_ && sw_pix_fmt == AV_PIX_FMT_P010LE && amber::ActiveSequence->width == params_.video_width &&
      height == amber::ActiveSequence->height && height % 2 == 0 && threads > 1 &&
      height >= threads * 64) {
    bool ok = true;
    for (int k = 0; k < threads && ok; k++) {
      ExportSwsBand b;
      b.y0 = (height * k / threads) & ~1;
      b.y1 = (k == threads - 1) ? height : ((height * (k + 1) / threads) & ~1);
      b.in0 = qMax(0, b.y0 - kSwsBandOverlap);
      b.in1 = qMin(height, b.y1 + kSwsBandOverlap);
      b.ctx = sws_getContext(params_.video_width, b.in1 - b.in0, AV_PIX_FMT_RGBA64LE,
                             params_.video_width, b.in1 - b.in0, sw_pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
      b.tmp = av_frame_alloc();
      if (b.ctx == nullptr || b.tmp == nullptr) {
        ok = false;
      } else {
        b.tmp->format = sw_pix_fmt;
        b.tmp->width = params_.video_width;
        b.tmp->height = b.in1 - b.in0;
        ok = av_frame_get_buffer(b.tmp, 0) >= 0;
      }
      sws_bands_.push_back(b);
    }
    if (!ok) {
      // Fall back to the single full-frame context
      for (auto& b : sws_bands_) {
        sws_freeContext(b.ctx);
        av_frame_free(&b.tmp);
      }
      sws_bands_.clear();
    }
  }

  return true;
}

bool ExportThread::SetupAudio() {
  // if video is disabled, no setup necessary
  if (!params_.audio_enabled) return true;

  // Find encoder for this codec
  acodec = avcodec_find_encoder(static_cast<AVCodecID>(params_.audio_codec));
  if (!acodec) {
    qCritical() << "Could not find audio encoder";
    export_error = tr("could not audio encoder for %1").arg(QString::number(params_.audio_codec));
    return false;
  }

  // Allocate audio stream
  audio_stream = avformat_new_stream(fmt_ctx, acodec);
  if (audio_stream == nullptr) {
    qCritical() << "Could not allocate audio stream";
    export_error = tr("could not allocate audio stream");
    return false;
  }

  // Set audio stream's ID to 1
  audio_stream->id = 1;

  // Sequences can carry an odd source rate (e.g. 47998 Hz), and encoders such as AAC refuse rates they do not list.
  // Use the supported rate closest to the requested one.
  {
    const int* rates = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 0, 0)
    const void* config = nullptr;
    int num_rates = 0;
    if (avcodec_get_supported_config(nullptr, acodec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &config, &num_rates) == 0) {
      rates = static_cast<const int*>(config);
    }
#else
    rates = acodec->supported_samplerates;
#endif
    if (rates != nullptr) {
      int best = 0;
      for (int i = 0; rates[i] != 0; i++) {
        if (best == 0 || qAbs(rates[i] - params_.audio_sampling_rate) < qAbs(best - params_.audio_sampling_rate)) {
          best = rates[i];
        }
      }
      if (best != 0 && best != params_.audio_sampling_rate) {
        qWarning() << "Audio encoder does not support" << params_.audio_sampling_rate << "Hz; using" << best << "Hz";
        params_.audio_sampling_rate = best;
      }
    }
  }

  // set sample rate to use for project
  audio_rendering_rate = params_.audio_sampling_rate;

  // Allocate encoding context
  acodec_ctx = avcodec_alloc_context3(acodec);
  if (!acodec_ctx) {
    qCritical() << "Could not find allocate audio encoding context";
    export_error = tr("could not allocate audio encoding context");
    return false;
  }

  // Set up encoding context
  acodec_ctx->codec_id = static_cast<AVCodecID>(params_.audio_codec);
  acodec_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
  acodec_ctx->sample_rate = params_.audio_sampling_rate;
  av_channel_layout_from_mask(&acodec_ctx->ch_layout, AV_CH_LAYOUT_STEREO);  // change this to support surround/mono sound in the future (this is what the user sets the output audio to)
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 0, 0)
  const enum AVSampleFormat *sample_fmts = nullptr;
  int num_sample_fmts = 0;
  avcodec_get_supported_config(nullptr, acodec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                               (const void **)&sample_fmts, &num_sample_fmts);
  acodec_ctx->sample_fmt = (sample_fmts && num_sample_fmts > 0) ? sample_fmts[0] : AV_SAMPLE_FMT_S16;
#else
  acodec_ctx->sample_fmt = acodec->sample_fmts ? acodec->sample_fmts[0] : AV_SAMPLE_FMT_S16;
#endif
  acodec_ctx->bit_rate = params_.audio_bitrate * 1000;

  acodec_ctx->time_base.num = 1;
  acodec_ctx->time_base.den = params_.audio_sampling_rate;
  audio_stream->time_base = acodec_ctx->time_base;

  if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
    acodec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  // Open encoder
  ret = avcodec_open2(acodec_ctx, acodec, nullptr);
  if (ret < 0) {
    qCritical() << "Could not open output audio encoder." << ret;
    export_error = tr("could not open output audio encoder (%1)").arg(QString::number(ret));
    return false;
  }

  // Copy parameters from the codec context (set up above) to the output stream
  ret = avcodec_parameters_from_context(audio_stream->codecpar, acodec_ctx);
  if (ret < 0) {
    qCritical() << "Could not copy audio encoder parameters to output stream." << ret;
    export_error = tr("could not copy audio encoder parameters to output stream (%1)").arg(QString::number(ret));
    return false;
  }

  // init audio resampler context
  {
    AVChannelLayout in_ch_layout = {};
    av_channel_layout_from_mask(&in_ch_layout, amber::ActiveSequence->audio_layout);
    swr_alloc_set_opts2(
          &swr_ctx,
          &acodec_ctx->ch_layout,
          acodec_ctx->sample_fmt,
          acodec_ctx->sample_rate,
          &in_ch_layout,
          AV_SAMPLE_FMT_S16,
          acodec_ctx->sample_rate,
          0,
          nullptr
          );
    av_channel_layout_uninit(&in_ch_layout);
  }
  ret = swr_init(swr_ctx);
  if (ret < 0) {
    qCritical() << "Could not initialize audio resampler." << ret;
    export_error = tr("could not initialize audio resampler (%1)").arg(QString::number(ret));
    return false;
  }

  // initialize raw audio frame
  audio_frame = av_frame_alloc();
  audio_frame->sample_rate = acodec_ctx->sample_rate;
  audio_frame->nb_samples = acodec_ctx->frame_size;

  if (audio_frame->nb_samples == 0) {
    // FIXME: Magic number. I don't know what to put here and truthfully I don't even know if it matters.
    audio_frame->nb_samples = 256;
  }

  // TODO change this to support surround/mono sound in the future (this is whatever format they're held in the internal buffer)
  av_channel_layout_from_mask(&audio_frame->ch_layout, AV_CH_LAYOUT_STEREO);

  audio_frame->format = AV_SAMPLE_FMT_S16;
  av_frame_make_writable(audio_frame);
  ret = av_frame_get_buffer(audio_frame, 0);
  if (ret < 0) {
    qCritical() << "Could not allocate audio buffer." << ret;
    export_error = tr("could not allocate audio buffer (%1)").arg(QString::number(ret));
    return false;
  }
  aframe_bytes = av_samples_get_buffer_size(nullptr, audio_frame->ch_layout.nb_channels, audio_frame->nb_samples, static_cast<AVSampleFormat>(audio_frame->format), 0);

  audio_pkt = av_packet_alloc();

  // init converted audio frame
  swr_frame = av_frame_alloc();
  av_channel_layout_copy(&swr_frame->ch_layout, &acodec_ctx->ch_layout);
  swr_frame->sample_rate = acodec_ctx->sample_rate;
  swr_frame->format = acodec_ctx->sample_fmt;
  swr_frame->nb_samples = acodec_ctx->frame_size;
  av_frame_get_buffer(swr_frame, 0);

  av_frame_make_writable(swr_frame);

  return true;
}

bool ExportThread::SetupContainer() {

  // Set up output context (using the filename as the format specification)

  avformat_alloc_output_context2(&fmt_ctx, nullptr, nullptr, c_filename);
  if (fmt_ctx == nullptr) {

    // Failed to create the output format context. Exit the export and throw an error.

    qCritical() << "Could not create output context";
    export_error = tr("could not create output format context");
    return false;
  }

  ret = avio_open(&fmt_ctx->pb, c_filename, AVIO_FLAG_WRITE);
  if (ret < 0) {

    // Failed to get a valid write handle for the exported file. Exit the export and throw an error.

    qCritical() << "Could not open output file." << ret;
    export_error = tr("could not open output file (%1)").arg(QString::number(ret));
    return false;
  }
  container_opened_ = true;

  return true;
}

// Returns true only when the file was fully written. False on any failure and on interrupt (an interrupt does not
// set export_error, so export_error alone can't tell).
// Time spent on each video step since the last log line, in microseconds. Used to find the export bottleneck.
struct ExportPhaseTimes {
  qint64 wait_us = 0;     // waiting for the GPU to finish the next frame
  qint64 convert_us = 0;  // 10-bit half-float to 16-bit conversion on the CPU
  qint64 scale_us = 0;    // swscale to the encoder's pixel format
  qint64 encode_us = 0;   // encoder call, including the VAAPI upload when used
  std::vector<qint64> convert_thread_us;  // time each conversion thread spent in the window
  int frames = 0;
  bool logged_setup = false;  // the frame size and thread count are logged once per export
};

// Returns the microseconds since the last call and restarts the timer. QElapsedTimer::restart() returns milliseconds,
// so the sub-millisecond part is read from nsecsElapsed() first.
static qint64 lap_us(QElapsedTimer& timer) {
  const qint64 us = timer.nsecsElapsed() / 1000;
  timer.restart();
  return us;
}

// Logs the average time per frame for each step every kPhaseLogInterval frames, then starts a new window
static const int kPhaseLogInterval = 30;
static void log_phase_times(ExportPhaseTimes& times) {
  if (++times.frames < kPhaseLogInterval) return;
  const double n = double(times.frames) * 1000.0;
  QString thread_text;
  for (qint64 us : times.convert_thread_us) thread_text += QString(" %1").arg(us / n, 0, 'f', 2);
  qInfo().noquote() << QString("Export timing, average ms per frame over %1 frames: "
                               "wait %2, convert %3, scale %4, encode %5, convert threads:%6")
                           .arg(times.frames)
                           .arg(times.wait_us / n, 0, 'f', 2)
                           .arg(times.convert_us / n, 0, 'f', 2)
                           .arg(times.scale_us / n, 0, 'f', 2)
                           .arg(times.encode_us / n, 0, 'f', 2)
                           .arg(thread_text);
  const bool logged_setup = times.logged_setup;
  times = ExportPhaseTimes();
  times.logged_setup = logged_setup;
}

bool ExportThread::Export()
{
  // Copy filename from QString to const char
  QByteArray ba = params_.filename.toUtf8();
  c_filename = qstrdup(ba.constData());

  // Set up file container
  if (!SetupContainer()) {
    return false;
  }

  // If video is enabled, set it up in the container now
  if (!SetupVideo()) {
    return false;
  }

  // If audio is enabled, set it up in the container now
  if (!SetupAudio()) {
    return false;
  }

  // Write the container header based on what's been set up above
  ret = avformat_write_header(fmt_ctx, nullptr);
  if (ret < 0) {

    // FFmpeg failed to write the header, so cancel the export and throw an error

    qCritical() << "Could not write output file header." << ret;
    export_error = tr("could not write output file header (%1)").arg(QString::number(ret));

    return false;
  }

  // Count audio samples in file (used for calculating PTS)
  long file_audio_samples = 0;

  // Set up timing variables, used for determining rendering ETA
  qint64 frame_start_time, frame_time, avg_time, eta, total_time = 0;

  // Frame counters - used for generating encoding statistics (e.g. average frame time, ETA, etc.)
  long remaining_frames, frame_count = 1;

  // Per-step timing for the export log (see log_phase_times)
  ExportPhaseTimes phase_times;
  QElapsedTimer phase_timer;

  // Set to false when a frame can't be produced or encoded: the file is then incomplete and must not be finalized
  bool frames_ok = true;

  // Create a dedicated render thread for export (viewer composites on main thread now)
  RenderThread* renderer = new RenderThread();
  if (gl_fallback_surface_) {
    renderer->setGlFallbackSurface(gl_fallback_surface_);
  }
  renderer->start(QThread::HighestPriority);
  connect(renderer, &RenderThread::ready, this, &ExportThread::wake);

  // The first frame is rendered before the loop. Each later frame is rendered during the previous frame's encode.
  if (params_.video_enabled) {
    int status = render_frame_sync(renderer, video_frame);
    if (status == 1) {
      renderer->cancel();
      delete renderer;
      goto cleanup_state;
    }
    if (status == 2) {
      export_error = tr("Could not initialize the GPU renderer.");
      frames_ok = false;
      goto cleanup_renderer;
    }
  }

  // Loop from now (set to the beginning frame earlier) to the end of the frame
  while (amber::ActiveSequence->playhead <= params_.end_frame && !interrupt_) {

    // Start timing how long this frame will take
    frame_start_time = QDateTime::currentMSecsSinceEpoch();

    // The frame this iteration encodes (the playhead moves on before the encode finishes)
    const long frame = amber::ActiveSequence->playhead;

    // If we're exporting audio, run compose_audio() which will write mixed audio to the internal audio buffer
    if (params_.audio_enabled) {
      waiting_for_audio_ = true;
      SetAudioWakeObject(this);
      amber::rendering::compose_audio(nullptr, amber::ActiveSequence.get(), 1, true);
    }

    // Video: this frame was rendered before the loop or during the previous iteration. Start rendering the next frame
    // now, so the GPU works while this frame is converted and encoded. The playhead moves first, because the render
    // reads the playhead when it runs.
    const bool render_next = params_.video_enabled && frame + 1 <= params_.end_frame;
    amber::ActiveSequence->playhead = frame + 1;
    if (render_next) {
      start_video_render(renderer, video_frame_alt);
    }

    // Timecode of this frame in seconds (used for timestamp calculations later on)
    double timecode_secs = double(frame - params_.start_frame) / amber::ActiveSequence->frame_rate;

    // If we're exporting video, construct an AVFrame in the destination codec's pixel format to convert the raw RGBA
    // OpenGL buffer to
    if (params_.video_enabled) {

      //
      // - I'm not sure why, but we have to alloc/free sws_frame every frame, or it breaks GIF exporting.
      // - (i.e. GIFs get stuck on the first frame)
      // - The same problem/solution can be seen here: https://stackoverflow.com/a/38997739
      // - Perhaps this is the intended way to use swscale, but it seems inefficient.
      // - Anyway, here we are.
      //

      // The 16-bit target reads back as half floats; turn them into 16-bit integers for swscale
      if (!phase_times.logged_setup) {
        phase_times.logged_setup = true;
        qInfo().noquote() << QString("Export frame size %1x%2 (sequence %3x%4), 10-bit %5, conversion threads %6")
                                 .arg(video_frame->width)
                                 .arg(video_frame->height)
                                 .arg(amber::ActiveSequence->width)
                                 .arg(amber::ActiveSequence->height)
                                 .arg(video_10bit_ ? "yes" : "no")
                                 .arg(conversion_threads());
      }

      phase_timer.restart();
      if (video_10bit_) {
        std::vector<qint64> frame_thread_us;
        half_float_to_unorm16(video_frame, &frame_thread_us);
        if (phase_times.convert_thread_us.size() < frame_thread_us.size()) {
          phase_times.convert_thread_us.resize(frame_thread_us.size(), 0);
        }
        for (size_t k = 0; k < frame_thread_us.size(); k++) {
          phase_times.convert_thread_us[k] += frame_thread_us[k];
        }
      }
      phase_times.convert_us += lap_us(phase_timer);

      // Construct destination pixel format frame
      sws_frame = av_frame_alloc();
      sws_frame->format = sw_pix_fmt;
      sws_frame->width = params_.video_width;
      sws_frame->height = params_.video_height;
      av_frame_get_buffer(sws_frame, 0);

      // Convert raw RGBA buffer to format expected by the encoder
      if (!sws_bands_.empty()) {
        scale_bands(sws_bands_, video_frame, sws_frame);
      } else {
        sws_scale(sws_ctx, video_frame->data, video_frame->linesize, 0, video_frame->height, sws_frame->data, sws_frame->linesize);
      }
      phase_times.scale_us += lap_us(phase_timer);
      sws_frame->pts = qRound(timecode_secs/av_q2d(vcodec_ctx->time_base));

      // VAAPI encoders need the frame copied into a GPU surface first
      AVFrame* encode_frame = sws_frame;
      if (hw_frames_ctx != nullptr) {
        encode_frame = av_frame_alloc();
        if (av_hwframe_get_buffer(hw_frames_ctx, encode_frame, 0) < 0 ||
            av_hwframe_transfer_data(encode_frame, sws_frame, 0) < 0) {
          qCritical() << "Could not upload frame to VAAPI surface";
          export_error = tr("could not upload frame to VAAPI surface");
          av_frame_free(&encode_frame);
          av_frame_free(&sws_frame);
          sws_frame = nullptr;
          frames_ok = false;
          goto cleanup_renderer;
        }
        encode_frame->pts = sws_frame->pts;
      }

      // Send frame to encoder
      bool encoded = Encode(fmt_ctx, vcodec_ctx, encode_frame, video_pkt, video_stream);
      if (encode_frame != sws_frame) av_frame_free(&encode_frame);
      av_frame_free(&sws_frame);
      sws_frame = nullptr;
      phase_times.encode_us += lap_us(phase_timer);
      if (!encoded) {
        frames_ok = false;
        goto cleanup_renderer;
      }
    }

    // If we're exporting audio, copy audio from the buffer into an AVFrame for encoding
    if (params_.audio_enabled) {

      while (waiting_for_audio_ && !interrupt_) {
        waitCond.wait(&mutex);
      }

      // Make sure nothing is writing while we're retrieving
      audio_write_lock.lock();

      // Check if the count of encoded samples exceeds the current Sequence playhead, in which case we don't need to
      // encode any audio at this moment
      while (!interrupt_ && file_audio_samples <= (timecode_secs*params_.audio_sampling_rate)) {

        // Copy samples from audio buffer to AVFrame
        int adjusted_read = audio_ibuffer_read.load() % audio_ibuffer_size;
        int copylen = qMin(aframe_bytes, audio_ibuffer_size-adjusted_read);
        memcpy(audio_frame->data[0], audio_ibuffer+adjusted_read, copylen);
        memset(audio_ibuffer+adjusted_read, 0, copylen);
        audio_ibuffer_read.fetch_add(copylen);

        // If we reached the end of the buffer without reaching the end of the frame, do another copy from the start
        // of the buffer
        if (copylen < aframe_bytes) {
          int remainder_len = aframe_bytes-copylen;
          memcpy(audio_frame->data[0]+copylen, audio_ibuffer, remainder_len);
          memset(audio_ibuffer, 0, remainder_len);
          audio_ibuffer_read.fetch_add(remainder_len);
        }

        // Convert raw audio samples to the destination codec's sample format
        swr_convert_frame(swr_ctx, swr_frame, audio_frame);

        // The timestamp is set to the current count of audio samples (since the audio stream's timebase is
        swr_frame->pts = file_audio_samples;

        // Send frame to encoder
        if (!Encode(fmt_ctx, acodec_ctx, swr_frame, audio_pkt, audio_stream)) {
          audio_write_lock.unlock();
          frames_ok = false;
          goto cleanup_renderer;
        }

        // Increment by the frame's number of samples
        file_audio_samples += swr_frame->nb_samples;
      }

      audio_write_lock.unlock();

    }

    // Generating encoding statistics (e.g. the time it took to encode this frame/estimated remaining time)
    frame_time = (QDateTime::currentMSecsSinceEpoch()-frame_start_time);
    total_time += frame_time;
    remaining_frames = (params_.end_frame - frame);
    avg_time = (total_time/frame_count);
    eta = (remaining_frames*avg_time);

    // Emit a signal for the percent of the sequence that's been encoded so far
    emit ProgressChanged(qRound((double(frame - params_.start_frame) / double(params_.end_frame - params_.start_frame)) * 100.0), eta);

    // The next frame has been rendering during this frame's encode. Wait for it, then swap it in as the current frame.
    if (render_next) {
      phase_timer.restart();
      int status = wait_for_render(renderer);
      phase_times.wait_us += lap_us(phase_timer);
      if (status == 0 && renderer->did_texture_fail()) {
        // The RenderThread failed. The playhead is already on the next frame, so render it again.
        status = render_frame_sync(renderer, video_frame_alt);
      }
      if (status == 1) {
        renderer->cancel();
        delete renderer;
        goto cleanup_state;
      }
      if (status == 2) {
        export_error = tr("Could not initialize the GPU renderer.");
        frames_ok = false;
        goto cleanup_renderer;
      }
      std::swap(video_frame, video_frame_alt);
    }

    // Increment frame count (used for generating encoding statistics above)
    frame_count++;
    log_phase_times(phase_times);
  }

cleanup_renderer:
  // Clean up export render thread
  renderer->cancel();
  delete renderer;

cleanup_state:
  // Clean up clip state.  Rendering state (audio_rendering flag + autorecovery
  // timer) is restored on the main thread in ExportDialog::export_thread_finished
  // — starting a QTimer from this thread triggers "Timers cannot be started
  // from another thread".
  close_active_clips(amber::ActiveSequence.get());

  if (interrupt_ || !frames_ok) {
    return false;
  }

  // If audio is enabled, flush the rest of the audio out of swresample
  if (params_.audio_enabled) {

    int flush_iter = 0;
    do {
      ret = swr_convert_frame(swr_ctx, swr_frame, nullptr);
      if (ret < 0 || swr_frame->nb_samples == 0) break;
      swr_frame->pts = file_audio_samples;
      if (!Encode(fmt_ctx, acodec_ctx, swr_frame, audio_pkt, audio_stream)) {
        return false;
      }
      file_audio_samples += swr_frame->nb_samples;
    } while (swr_frame->nb_samples > 0 && ++flush_iter < 1000);

  }

  if (interrupt_) {
    return false;
  }

  // Flush remaining packets out of video and audio encoders by sending a null frame
  if (params_.video_enabled) {
    Encode(fmt_ctx, vcodec_ctx, nullptr, video_pkt, video_stream);
  }
  if (params_.audio_enabled) {
    Encode(fmt_ctx, acodec_ctx, nullptr, audio_pkt, audio_stream);
  }

  // Write container trailer
  ret = av_write_trailer(fmt_ctx);
  if (ret < 0) {
    qCritical() << "Could not write output file trailer." << ret;
    export_error = tr("could not write output file trailer (%1)").arg(QString::number(ret));
    return false;
  }

  emit ProgressChanged(100, 0);
  return true;
}

void ExportThread::Cleanup()
{
  if (fmt_ctx != nullptr) {
    avio_closep(&fmt_ctx->pb);
    avformat_free_context(fmt_ctx);
  }

  if (acodec_ctx != nullptr) {
    avcodec_free_context(&acodec_ctx);
  }

  if (audio_frame != nullptr) {
    av_frame_free(&audio_frame);
  }

  if (audio_pkt != nullptr) {
    av_packet_free(&audio_pkt);
  }

  if (vcodec_ctx != nullptr) {
    avcodec_free_context(&vcodec_ctx);
  }

  if (video_frame != nullptr) {
    av_frame_free(&video_frame);
  }

  if (video_frame_alt != nullptr) {
    av_frame_free(&video_frame_alt);
  }

  if (video_pkt != nullptr) {
    av_packet_free(&video_pkt);
  }

  for (auto& b : sws_bands_) {
    sws_freeContext(b.ctx);
    av_frame_free(&b.tmp);
  }
  sws_bands_.clear();

  if (sws_ctx != nullptr) {
    sws_freeContext(sws_ctx);
  }

  if (swr_ctx != nullptr) {
    swr_free(&swr_ctx);
  }

  if (swr_frame != nullptr) {
    av_frame_free(&swr_frame);
  }

  if (sws_frame != nullptr) {
    av_frame_free(&sws_frame);
  }

  av_buffer_unref(&hw_frames_ctx);
  av_buffer_unref(&hw_device_ctx);

  delete [] c_filename;
}

void ExportThread::run() {
  // Pause/seek/audio_rendering_rate are set on the main thread in
  // ExportDialog before start() — calling UI methods (pausePlayback,
  // seekPlayhead) from this thread would trigger widget repaints on the
  // wrong thread, crashing the OpenGL backend.

  // Lock mutex (used for thread synchronizations)
  mutex.lock();

  // Run export function (returns false on failure and on interrupt)
  bool succeeded = Export();

  mutex.unlock();

  // Clean up anything that was allocated in Export() (whether it succeeded or not)
  Cleanup();

  // Don't leave a truncated file behind. Cleanup() has already closed the AVIO handle, which Windows needs before
  // the file can be deleted.
  if (!succeeded && container_opened_) {
    QFile::remove(params_.filename);
  }

#ifdef __GLIBC__
  // Return freed heap pages to the OS. Export churns through many short-lived
  // allocations (QRhi resources, YUV plane copies) and glibc keeps those pages
  // in its free lists, inflating RSS after export.
  malloc_trim(0);
#endif
}

const QString &ExportThread::GetError() {
  return export_error;
}

bool ExportThread::WasInterrupted()
{
  return interrupt_;
}

void ExportThread::Interrupt()
{
  mutex.lock();
  interrupt_ = true;
  waitCond.wakeAll();
  mutex.unlock();
}

void ExportThread::play_wake()
{
  mutex.lock();
  waiting_for_audio_ = false;
  waitCond.wakeAll();
  mutex.unlock();
}

void ExportThread::start_video_render(RenderThread* renderer, AVFrame* buf) {
  render_complete_ = false;
  renderer->start_render(amber::ActiveSequence.get(), 1, nullptr, buf->data[0],
                         buf->linesize[0] / (video_10bit_ ? 8 : 4), 0, false, video_10bit_);
}

int ExportThread::wait_for_render(RenderThread* renderer) {
  // Predicate guards against a lost wakeup
  while (!render_complete_ && !interrupt_) {
    waitCond.wait(&mutex);
  }
  if (interrupt_) {
    return 1;
  }
  // QRhi creation failed: ready() still fired, but there is no frame and there never will be
  if (renderer->rhi_failed()) {
    return 2;
  }
  return 0;
}

int ExportThread::render_frame_sync(RenderThread* renderer, AVFrame* buf) {
  int status = 0;
  do {
    start_video_render(renderer, buf);
    status = wait_for_render(renderer);
    if (status != 0) {
      return status;
    }
    // If the RenderThread failed, do another render
  } while (renderer->did_texture_fail());
  return 0;
}

void ExportThread::wake() {
  mutex.lock();
  render_complete_ = true;
  waitCond.wakeAll();
  mutex.unlock();
}
