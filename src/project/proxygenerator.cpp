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

#include "proxygenerator.h"

#include "core/path.h"
#include "global/config.h"
#include "project/footage.h"
#include "project/previewgenerator.h"
#include "ui/mainwindow.h"
#include "ui/mediaiconservice.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStatusBar>
#include <QtMath>

#include <QDebug>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

constexpr double kProxySizeMultipliers[] = {1.0, 0.5, 0.25, 0.125, 0.0625};

ProxyGenerator::ProxyGenerator() = default;

// Chooses the VAAPI surface format when the decoder offers it. Otherwise returns the first software format so
// decoding still proceeds in software.
static enum AVPixelFormat pick_vaapi_format(AVCodecContext*, const enum AVPixelFormat* formats) {
  for (const enum AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; f++) {
    if (*f == AV_PIX_FMT_VAAPI) return *f;
  }
  return formats[0];
}

static AVCodecContext* open_decoder(const AVCodecParameters* par, const AVCodec* codec, bool nvdec,
                                    AVBufferRef* hw_device = nullptr) {
  if (codec == nullptr) return nullptr;

  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (ctx == nullptr) return nullptr;

  avcodec_parameters_to_context(ctx, par);
  if (nvdec) {
    av_opt_set_int(ctx->priv_data, "gpu_output_format", AV_PIX_FMT_NV12, 0);
    ctx->color_range = AVCOL_RANGE_MPEG;
  }
  if (hw_device != nullptr) {
    ctx->hw_device_ctx = av_buffer_ref(hw_device);
    ctx->get_format = pick_vaapi_format;
  }

  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    avcodec_free_context(&ctx);
    return nullptr;
  }
  return ctx;
}

// Proxies are named after the source file plus a short hash of its full path, so same-named clips from different
// folders never overwrite each other.
static QString proxy_path_for(const Footage* footage) {
  QByteArray hash = QCryptographicHash::hash(footage->url.toUtf8(), QCryptographicHash::Md5).toHex().left(8);
  QString name = QFileInfo(footage->url).baseName() + "_" + QString::fromLatin1(hash) + ".mov";
  return QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("Proxy")).filePath(name);
}

void ProxyGenerator::transcode(const ProxyInfo& info) {
  Footage* footage = info.media->to_footage();

  current_progress = 0.0;

  AVDictionary* format_opts = nullptr;
  if (footage->start_number > 0) {
    av_dict_set(&format_opts, "start_number", QString::number(footage->start_number).toUtf8(), 0);
  }

  AVFormatContext* input_fmt_ctx = nullptr;
  int open_ret = avformat_open_input(&input_fmt_ctx, footage->url.toUtf8(), nullptr, &format_opts);
  av_dict_free(&format_opts);
  if (open_ret < 0) {
    qCritical() << "Proxy: could not open input" << footage->url;
    return;
  }

  if (avformat_find_stream_info(input_fmt_ctx, nullptr) < 0) {
    qCritical() << "Proxy: could not find stream info" << footage->url;
    avformat_close_input(&input_fmt_ctx);
    return;
  }

  // Check before creating the output file so audio-only media doesn't leave an empty proxy behind
  bool has_video = false;
  for (unsigned int i = 0; i < input_fmt_ctx->nb_streams; i++) {
    const AVStream* s = input_fmt_ctx->streams[i];
    if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(s->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      has_video = true;
    }
  }
  if (!has_video) {
    qWarning() << "Proxy: no video stream to transcode in" << footage->url;
    avformat_close_input(&input_fmt_ctx);
    return;
  }

  AVFormatContext* output_fmt_ctx = nullptr;
  avformat_alloc_output_context2(&output_fmt_ctx, nullptr, nullptr, info.path.toUtf8());
  if (output_fmt_ctx == nullptr) {
    qCritical() << "Proxy: could not allocate output context";
    avformat_close_input(&input_fmt_ctx);
    return;
  }

  if (avio_open(&output_fmt_ctx->pb, info.path.toUtf8(), AVIO_FLAG_WRITE) < 0) {
    qCritical() << "Proxy: could not open output" << info.path;
    avformat_close_input(&input_fmt_ctx);
    avformat_free_context(output_fmt_ctx);
    return;
  }

  const int nb_streams = int(input_fmt_ctx->nb_streams);

  QVector<AVCodecContext*> input_streams(nb_streams, nullptr);
  QVector<AVCodecContext*> output_streams(nb_streams, nullptr);
  QVector<SwsContext*> sws_contexts(nb_streams, nullptr);

  // Pixel format each scaler was built for. Decoded frames can arrive in a different format than codecpar reports
  // (VAAPI and NVDEC output), so the scaler is rebuilt whenever this changes.
  QVector<int> sws_src_formats(nb_streams, AV_PIX_FMT_NONE);

  // Shared VAAPI device for all video streams in this file, created on first use
  AVBufferRef* vaapi_device = nullptr;
  bool vaapi_failed = false;

  // Decoder that produced the video frames, recorded on the footage for display
  QString decode_method = "Software";

  // Maps an INPUT stream index to its OUTPUT stream index, or -1 if this input stream is dropped entirely (not written
  // to the proxy at all).
  QVector<int> out_index(nb_streams, -1);

  for (int i = 0; i < nb_streams; i++) {
    AVStream* in_stream = input_fmt_ctx->streams[i];
    AVMediaType type = in_stream->codecpar->codec_type;

    // Keep only video and audio: some cameras (DJI included) embed proprietary metadata/timecode tracks the mov muxer
    // can't write, which corrupts the whole output file's header.
    if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) continue;

    // Embedded cover art (e.g. DJI's MJPEG preview image) is a still picture, not video content.
    if (type == AVMEDIA_TYPE_VIDEO && (in_stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;

    if (type == AVMEDIA_TYPE_AUDIO) {
      // Audio passthrough. Clear codec_tag so the muxer assigns the right tag for THIS container rather than carrying
      // over one that may be invalid outside the source file's original container.
      AVStream* out_stream = avformat_new_stream(output_fmt_ctx, nullptr);
      out_stream->id = in_stream->id;
      out_index[i] = out_stream->index;
      avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
      out_stream->codecpar->codec_tag = 0;
      continue;
    }

    AVCodecContext* dec_ctx = nullptr;
    const char* cuvid_name = nullptr;
    if (in_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
      cuvid_name = "hevc_cuvid";
    } else if (in_stream->codecpar->codec_id == AV_CODEC_ID_H264) {
      cuvid_name = "h264_cuvid";
    }
    if (cuvid_name != nullptr) {
      dec_ctx = open_decoder(in_stream->codecpar, avcodec_find_decoder_by_name(cuvid_name), true);
      if (dec_ctx == nullptr) {
        qWarning() << "Proxy:" << cuvid_name << "unavailable, trying VAAPI";
      } else {
        decode_method = "NVDEC";
      }
    }
    if (dec_ctx == nullptr && amber::CurrentConfig.hardware_decoding && !vaapi_failed) {
      if (vaapi_device == nullptr &&
          av_hwdevice_ctx_create(&vaapi_device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0) {
        qWarning() << "Proxy: could not create VAAPI device, falling back to software decoder";
        vaapi_failed = true;
      }
      if (vaapi_device != nullptr) {
        dec_ctx =
            open_decoder(in_stream->codecpar, avcodec_find_decoder(in_stream->codecpar->codec_id), false, vaapi_device);
        if (dec_ctx == nullptr) {
          qWarning() << "Proxy: VAAPI cannot decode stream" << i << ", falling back to software decoder";
        }
      }
    }
    if (dec_ctx == nullptr) {
      dec_ctx = open_decoder(in_stream->codecpar, avcodec_find_decoder(in_stream->codecpar->codec_id), false);
    }
    if (dec_ctx == nullptr) {
      qCritical() << "Proxy: could not open decoder for video stream" << i;
      skip = true;
      continue;
    }

    auto open_encoder = [&](const AVCodec* codec) -> AVCodecContext* {
      if (codec == nullptr) return nullptr;

      AVCodecContext* ctx = avcodec_alloc_context3(codec);
      if (ctx == nullptr) return nullptr;

      ctx->codec_type = AVMEDIA_TYPE_VIDEO;
      ctx->codec_id = AV_CODEC_ID_PRORES;
      ctx->profile = AV_PROFILE_PRORES_PROXY;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 0, 0)
      const enum AVPixelFormat* pix_fmts = nullptr;
      int num_pix_fmts = 0;
      avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void**)&pix_fmts,
                                   &num_pix_fmts);
      ctx->pix_fmt = (pix_fmts && num_pix_fmts > 0) ? pix_fmts[0] : AV_PIX_FMT_YUV422P;
#else
      ctx->pix_fmt = codec->pix_fmts ? codec->pix_fmts[0] : AV_PIX_FMT_YUV422P;
#endif

      ctx->width = qFloor(dec_ctx->width * info.size_multiplier);
      ctx->height = qFloor(dec_ctx->height * info.size_multiplier);
      ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
      ctx->framerate = dec_ctx->framerate;
      ctx->time_base = in_stream->time_base;

      if (output_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
      }

      AVDictionary* opts = nullptr;
      av_dict_set(&opts, "threads", "auto", 0);
      int ret = avcodec_open2(ctx, codec, &opts);
      av_dict_free(&opts);
      if (ret < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
      }
      return ctx;
    };

    AVCodecContext* enc_ctx = open_encoder(avcodec_find_encoder(AV_CODEC_ID_PRORES));
    if (enc_ctx == nullptr) {
      qCritical() << "Proxy: could not open encoder";
      avcodec_free_context(&dec_ctx);
      skip = true;
      continue;
    }

    AVStream* out_stream = avformat_new_stream(output_fmt_ctx, nullptr);
    out_stream->id = in_stream->id;
    out_stream->time_base = in_stream->time_base;
    out_index[i] = out_stream->index;
    avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);

    input_streams[i] = dec_ctx;
    output_streams[i] = enc_ctx;
  }

  if (avformat_write_header(output_fmt_ctx, nullptr) < 0) {
    qCritical() << "Proxy: could not write header";
    skip = true;
  }

  AVPacket* packet = av_packet_alloc();
  AVFrame* dec_frame = av_frame_alloc();
  AVFrame* sw_frame = av_frame_alloc();
  AVFrame* enc_frame = av_frame_alloc();

  int64_t total_duration = input_fmt_ctx->duration > 0 ? input_fmt_ctx->duration : 1;

  auto encode_decoded_frames = [&](int stream_index) {
    while (avcodec_receive_frame(input_streams.at(stream_index), dec_frame) >= 0) {
      // Frame timestamps are in the stream's time base but the total duration is in AV_TIME_BASE (microseconds), so
      // convert the timestamp before comparing them
      int64_t pts = dec_frame->best_effort_timestamp;
      if (pts != AV_NOPTS_VALUE) {
        int64_t pts_us = av_rescale_q(pts, input_fmt_ctx->streams[stream_index]->time_base, AV_TIME_BASE_Q);
        current_progress = qBound(0.0, double(pts_us) / double(total_duration) * 100.0, 100.0);
      }

      AVFrame* src_frame = dec_frame;
      if (dec_frame->format == AV_PIX_FMT_VAAPI) {
        // VAAPI frames live in GPU memory, so copy them to system memory before scaling
        decode_method = "VAAPI";
        if (av_hwframe_transfer_data(sw_frame, dec_frame, 0) < 0) {
          qCritical() << "Proxy: could not transfer VAAPI frame to system memory";
          av_frame_unref(dec_frame);
          skip = true;
          break;
        }
        av_frame_copy_props(sw_frame, dec_frame);
        src_frame = sw_frame;
      }

      AVCodecContext* enc = output_streams.at(stream_index);
      AVFrame* frame_to_encode = src_frame;

      bool convert =
          (src_frame->format != enc->pix_fmt || src_frame->width != enc->width || src_frame->height != enc->height);
      if (convert) {
        if (sws_contexts.at(stream_index) == nullptr || sws_src_formats.at(stream_index) != src_frame->format) {
          sws_freeContext(sws_contexts[stream_index]);
          sws_contexts[stream_index] =
              sws_getContext(src_frame->width, src_frame->height, static_cast<AVPixelFormat>(src_frame->format),
                             enc->width, enc->height, enc->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
          sws_src_formats[stream_index] = src_frame->format;
          if (sws_contexts.at(stream_index) == nullptr) {
            qCritical() << "Proxy: could not create scaler for decoded frame format" << src_frame->format;
            av_frame_unref(sw_frame);
            av_frame_unref(dec_frame);
            skip = true;
            break;
          }
        }

        enc_frame->width = enc->width;
        enc_frame->height = enc->height;
        enc_frame->format = enc->pix_fmt;
        av_frame_get_buffer(enc_frame, 32);

        sws_scale(sws_contexts.at(stream_index), src_frame->data, src_frame->linesize, 0, src_frame->height,
                  enc_frame->data, enc_frame->linesize);
        enc_frame->pts = src_frame->pts;
        frame_to_encode = enc_frame;
      }

      avcodec_send_frame(enc, frame_to_encode);

      if (convert) {
        av_frame_unref(enc_frame);
      }
      if (src_frame == sw_frame) {
        av_frame_unref(sw_frame);
      }
      av_frame_unref(dec_frame);

      AVPacket* out_pkt = av_packet_alloc();
      while (avcodec_receive_packet(output_streams.at(stream_index), out_pkt) >= 0) {
        out_pkt->stream_index = out_index[stream_index];
        av_packet_rescale_ts(out_pkt, output_streams.at(stream_index)->time_base,
                             output_fmt_ctx->streams[out_index[stream_index]]->time_base);
        av_interleaved_write_frame(output_fmt_ctx, out_pkt);
        av_packet_unref(out_pkt);
      }
      av_packet_free(&out_pkt);
    }
  };

  while (!skip && av_read_frame(input_fmt_ctx, packet) >= 0) {
    int stream_index = packet->stream_index;

    if (out_index[stream_index] < 0) {
      av_packet_unref(packet);
      continue;
    }

    if (input_streams.at(stream_index) == nullptr) {
      // Audio passthrough
      av_packet_rescale_ts(packet, input_fmt_ctx->streams[stream_index]->time_base,
                           output_fmt_ctx->streams[out_index[stream_index]]->time_base);
      packet->stream_index = out_index[stream_index];
      packet->pos = -1;
      av_interleaved_write_frame(output_fmt_ctx, packet);
      av_packet_unref(packet);
      continue;
    }

    avcodec_send_packet(input_streams.at(stream_index), packet);
    av_packet_unref(packet);

    encode_decoded_frames(stream_index);

    if (skip) break;
  }

  if (!skip) {
    for (int i = 0; i < nb_streams; i++) {
      if (input_streams.at(i) == nullptr) continue;
      avcodec_send_packet(input_streams.at(i), nullptr);
      encode_decoded_frames(i);
    }
  }

  bool success = false;
  if (!skip) {
    for (int i = 0; i < nb_streams; i++) {
      if (output_streams.at(i) == nullptr || out_index[i] < 0) continue;

      avcodec_send_frame(output_streams.at(i), nullptr);
      AVPacket* out_pkt = av_packet_alloc();
      while (avcodec_receive_packet(output_streams.at(i), out_pkt) >= 0) {
        out_pkt->stream_index = out_index[i];
        av_packet_rescale_ts(out_pkt, output_streams.at(i)->time_base,
                             output_fmt_ctx->streams[out_index[i]]->time_base);
        av_interleaved_write_frame(output_fmt_ctx, out_pkt);
        av_packet_unref(out_pkt);
      }
      av_packet_free(&out_pkt);
    }
    av_write_trailer(output_fmt_ctx);

    footage->proxy = true;
    footage->proxy_path = info.path;
    footage->proxy_method = decode_method;
    success = true;
  }

  av_packet_free(&packet);
  av_frame_free(&dec_frame);
  av_frame_free(&sw_frame);
  av_frame_free(&enc_frame);

  for (int i = 0; i < nb_streams; i++) {
    if (input_streams.at(i)) avcodec_free_context(&input_streams[i]);
    if (output_streams.at(i)) avcodec_free_context(&output_streams[i]);
    if (sws_contexts.at(i)) sws_freeContext(sws_contexts.at(i));
  }
  // decoder contexts hold their own references to the device, so it can be released once they are freed
  av_buffer_unref(&vaapi_device);

  avio_closep(&output_fmt_ctx->pb);
  avformat_free_context(output_fmt_ctx);
  avformat_close_input(&input_fmt_ctx);

  if (!success) QFile::remove(info.path);

  current_progress = 100.0;
}

void ProxyGenerator::run() {
  while (true) {
    ProxyInfo info;
    {
      QMutexLocker lock(&mutex);

      // wait for queue() to add work, or cancel() to stop the thread
      while (proxy_queue.isEmpty() && !cancelled) {
        waitCond.wait(&mutex);
      }
      if (cancelled) break;

      info = proxy_queue.first();
      skip = false;
    }

    QFileInfo(info.path).dir().mkpath(".");

    transcode(info);

    Footage* footage = info.media->to_footage();

    bool queued_again = false;
    {
      QMutexLocker lock(&mutex);
      proxy_queue.removeFirst();
      for (const ProxyInfo& queued : proxy_queue) {
        if (queued.media == info.media) queued_again = true;
      }
    }

    // a newer request for this footage is still queued and owns the proxy state
    if (!queued_again && footage->proxy_path.isEmpty()) footage->proxy = false;

    amber::media_icon_service->SetMediaIcon(info.media,
                                            footage->video_tracks.isEmpty() ? ICON_TYPE_AUDIO : ICON_TYPE_VIDEO);

    if (cancelled) break;
  }
}

void ProxyGenerator::queue_media(Media* media, bool force) {
  Footage* footage = media->to_footage();

  bool has_proxy = !footage->proxy_path.isEmpty() && QFileInfo::exists(footage->proxy_path);
  bool in_progress = footage->proxy && footage->proxy_path.isEmpty();
  if (!force && (has_proxy || in_progress)) return;

  ProxyInfo info;
  info.media = media;
  info.keep_alive = media->parentItem() ? media->parentItem()->get_shared_ptr(media) : MediaPtr();
  info.size_multiplier = kProxySizeMultipliers[qBound(0, amber::CurrentConfig.proxy_size_index, 4)];
  info.path = proxy_path_for(footage);

  footage->proxy = true;
  footage->proxy_path.clear();
  footage->proxy_method.clear();

  queue(info);
}

void ProxyGenerator::queue(const ProxyInfo& info) {
  QMutexLocker lock(&mutex);

  if (!proxy_queue.isEmpty() && proxy_queue.first().media == info.media) {
    // if the thread is currently processing a proxy with the same footage, abort it
    skip = true;
  }

  // scan through the rest of the queue for another proxy with the same footage (start with 1 since we already
  // processed first())
  for (int i = 1; i < proxy_queue.size(); i++) {
    if (proxy_queue.at(i).media == info.media) {
      // found a duplicate, assume the one we're queuing now overrides and delete it
      proxy_queue.removeAt(i);
      i--;
    }
  }

  proxy_queue.append(info);

  waitCond.wakeAll();
}

void ProxyGenerator::cancel() {
  {
    QMutexLocker lock(&mutex);
    cancelled = true;
    skip = true;
    waitCond.wakeAll();
  }

  wait();
}

double ProxyGenerator::get_proxy_progress(Media* m) {
  QMutexLocker lock(&mutex);
  if (!proxy_queue.isEmpty() && proxy_queue.first().media == m) {
    return current_progress;
  }
  return 0.0;
}

// proxy generator is a global omnipotent entity
ProxyGenerator amber::proxy_generator;
