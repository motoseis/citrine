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

#include "renderthread.h"

#include <QApplication>
#include <QImage>
#include <QDateTime>
#include <QDebug>
#include <QFile>
// Qt may set QT_CONFIG(vulkan) even without a real Vulkan SDK (e.g. macOS
// Homebrew Qt with MoltenVK headers but no SDK on the build machine).
// After including <QVulkanInstance>, check for VK_VERSION_1_0 to know if
// the real vulkan.h was found — without it, QVulkanInstance is stub-only.
#if QT_CONFIG(vulkan)
#include <QVulkanInstance>
#endif
#if QT_CONFIG(vulkan) && defined(VK_VERSION_1_0)
#define AMBER_HAS_VULKAN 1
#endif

#include "global/config.h"
#include "rendering/renderfunctions.h"
#include "engine/sequence.h"

static QShader loadQsb(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    qCritical() << "Failed to load shader:" << path;
    return {};
  }
  QShader s = QShader::fromSerialized(f.readAll());
  if (!s.isValid()) {
    qCritical() << "Invalid .qsb shader:" << path;
  }
  return s;
}

QMutex RenderThread::deferred_delete_mutex_;
QVector<QRhiResource*> RenderThread::deferred_delete_queue_;

void RenderThread::DeferRhiResourceDeletion(QRhiResource* res) {
  if (res == nullptr) return;
  QMutexLocker lock(&deferred_delete_mutex_);
  deferred_delete_queue_.append(res);
}

void RenderThread::DeferRhiResourceDeletion(const QVector<QRhiResource*>& resources) {
  QMutexLocker lock(&deferred_delete_mutex_);
  for (auto* res : resources) {
    if (res) deferred_delete_queue_.append(res);
  }
}

static void copy_yuv_planes(const QRhiReadbackResult& luma, const QRhiReadbackResult& chroma,
                            const ExportPlanes& dst, QSize size);

QRhiTexture::Format RenderThread::IntermediateFormat() {
  return amber::HighPrecisionIntermediates.load() ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
}

void RenderThread::drainDeferredDeletes() {
  QMutexLocker lock(&deferred_delete_mutex_);
  qDeleteAll(deferred_delete_queue_);
  deferred_delete_queue_.clear();
}

RenderThread::RenderThread()
    : front_buffer_switcher(false), queued(false) {
}

RenderThread::~RenderThread() {}

void RenderThread::setGlFallbackSurface(QOffscreenSurface* surface) {
  fallbackSurface_ = surface;
  owns_fallback_surface_ = false;
}

void RenderThread::run() {
  wait_lock_.lock();

  while (running) {
    if (!queued) {
      wait_cond_.wait(&wait_lock_);
    }
    if (!running) {
      break;
    }
    queued = false;

    // Create QRhi if not yet initialized
    if (rhi_ == nullptr) {
      RhiBackend backend = amber::CurrentRuntimeConfig.rhi_backend;

      switch (backend) {
#if AMBER_HAS_VULKAN
        case RhiBackend::Vulkan: {
          auto* vi = static_cast<QVulkanInstance*>(amber::CurrentRuntimeConfig.vulkan_instance);
          if (vi && vi->isValid()) {
            QRhiVulkanInitParams vkParams;
            vkParams.inst = vi;
            rhi_ = QRhi::create(QRhi::Vulkan, &vkParams);
          }
          if (!rhi_) qWarning() << "Vulkan QRhi creation failed, falling back to OpenGL";
          break;
        }
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
        case RhiBackend::Metal: {
          QRhiMetalInitParams mtlParams;
          rhi_ = QRhi::create(QRhi::Metal, &mtlParams);
          if (!rhi_) qWarning() << "Metal QRhi creation failed, falling back to OpenGL";
          break;
        }
#endif
#if defined(Q_OS_WIN)
        case RhiBackend::D3D12: {
          QRhiD3D12InitParams d3d12Params;
          rhi_ = QRhi::create(QRhi::D3D12, &d3d12Params);
          if (!rhi_) {
            qWarning() << "D3D12 QRhi creation failed, trying D3D11";
            QRhiD3D11InitParams d3d11Params;
            rhi_ = QRhi::create(QRhi::D3D11, &d3d11Params);
            if (!rhi_) qWarning() << "D3D11 also failed, falling back to OpenGL";
          }
          break;
        }
        case RhiBackend::D3D11: {
          QRhiD3D11InitParams d3d11Params;
          rhi_ = QRhi::create(QRhi::D3D11, &d3d11Params);
          if (!rhi_) qWarning() << "D3D11 QRhi creation failed, falling back to OpenGL";
          break;
        }
#endif
        default:
          break;
      }

      // OpenGL fallback.  Prefer a surface pre-created on the GUI thread
      // (via setGlFallbackSurface) to avoid the "QWindow outside gui thread"
      // warning on Wayland.  Fall back to creating one here if the caller
      // didn't provide one (produces a warning but still works).
      if (!rhi_) {
        if (!fallbackSurface_) {
          fallbackSurface_ = QRhiGles2InitParams::newFallbackSurface();
          owns_fallback_surface_ = true;
        }
        QRhiGles2InitParams glParams;
        glParams.fallbackSurface = fallbackSurface_;
        rhi_ = QRhi::create(QRhi::OpenGLES2, &glParams);
      }

      // Last resort: software Vulkan (llvmpipe) if OpenGL also failed
#if AMBER_HAS_VULKAN
      if (!rhi_ && amber::CurrentRuntimeConfig.vulkan_is_software) {
        auto* vi = static_cast<QVulkanInstance*>(amber::CurrentRuntimeConfig.vulkan_instance);
        if (vi && vi->isValid()) {
          qWarning() << "OpenGL failed, falling back to software Vulkan (llvmpipe)";
          QRhiVulkanInitParams vkParams;
          vkParams.inst = vi;
          rhi_ = QRhi::create(QRhi::Vulkan, &vkParams);
        }
      }
#endif

      if (!rhi_) {
        qCritical() << "Failed to create QRhi with any backend";
        // Still signal the waiter (ExportThread blocks until ready()); it checks rhi_failed(). The viewer only
        // repaints the empty frame it already had.
        rhi_failed_ = true;
        emit ready();
        continue;
      }
      // A later retry succeeded: clear the flag so a stale failure doesn't linger for rhi_failed() callers
      rhi_failed_ = false;
      qInfo() << "QRhi initialized, backend:" << rhi_->backendName()
              << "driver:" << rhi_->driverInfo().deviceName;

      // 10-bit YUV planes are uploaded as 16-bit textures; some GL ES drivers can't sample them
      amber::Yuv10BitSupported = rhi_->isTextureFormatSupported(QRhiTexture::R16) &&
                                 rhi_->isTextureFormatSupported(QRhiTexture::RG16);
      qInfo() << "10-bit YUV decode supported:" << amber::Yuv10BitSupported.load();
      amber::Yuv10BitExportSupported = rhi_->isTextureFormatSupported(QRhiTexture::R16, QRhiTexture::RenderTarget) &&
                                       rhi_->isTextureFormatSupported(QRhiTexture::RG16, QRhiTexture::RenderTarget);

      // Compositing in RGBA16F needs the backend to render into it; otherwise every target stays RGBA8
      amber::HighPrecisionIntermediates = rhi_->isTextureFormatSupported(QRhiTexture::RGBA16F);
      qInfo() << "16-bit compositing targets:" << amber::HighPrecisionIntermediates.load();

      // Load core shaders from QRC
      passthroughVert_ = loadQsb(QStringLiteral(":/shaders/common.vert.qsb"));
      passthroughFrag_ = loadQsb(QStringLiteral(":/shaders/passthrough.frag.qsb"));
      blendingFrag_ = loadQsb(QStringLiteral(":/shaders/blending.frag.qsb"));
      premultiplyFrag_ = loadQsb(QStringLiteral(":/shaders/premultiply.frag.qsb"));
      yuvFrag_ = loadQsb(QStringLiteral(":/shaders/yuv2rgb.frag.qsb"));
      resolveFrag_ = loadQsb(QStringLiteral(":/shaders/resolve8.frag.qsb"));
      downscaleFrag_ = loadQsb(QStringLiteral(":/shaders/downscale.frag.qsb"));
      yuv10LumaFrag_ = loadQsb(QStringLiteral(":/shaders/yuv10_luma.frag.qsb"));
      yuv10ChromaFrag_ = loadQsb(QStringLiteral(":/shaders/yuv10_chroma.frag.qsb"));

      // Sampler
      sampler_ = rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                   QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
      sampler_->create();
    }

    if (rhi_ != nullptr) {
      // Recreate buffers if sequence size or divider changed
      int target_w = qMax(1, seq->width / divider_);
      int target_h = qMax(1, seq->height / divider_);
      if (target_w != tex_width || target_h != tex_height) {
        delete_buffers();
        tex_width = target_w;
        tex_height = target_h;
      }

      // Create front buffers (double-buffered compositing targets)
      if (front_tex_[0] == nullptr) {
        for (int i = 0; i < 2; i++) {
          front_tex_[i] = rhi_->newTexture(IntermediateFormat(), QSize(tex_width, tex_height),
                                           1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
          front_tex_[i]->create();

          // Compositing RT: preserves contents between clips
          front_rt_[i] = rhi_->newTextureRenderTarget(
              {front_tex_[i]}, QRhiTextureRenderTarget::PreserveColorContents);
          if (i == 0) {
            front_rpd_ = front_rt_[i]->newCompatibleRenderPassDescriptor();
          }
          front_rt_[i]->setRenderPassDescriptor(front_rpd_);
          front_rt_[i]->create();

          // Clear RT: same texture, no PreserveColorContents (beginPass actually clears)
          front_rt_clear_[i] = rhi_->newTextureRenderTarget({front_tex_[i]});
          if (i == 0) {
            front_clear_rpd_ = front_rt_clear_[i]->newCompatibleRenderPassDescriptor();
          }
          front_rt_clear_[i]->setRenderPassDescriptor(front_clear_rpd_);
          front_rt_clear_[i]->create();
        }

        // 8-bit target for the readback; only needed when the compositing targets are 16-bit
        if (IntermediateFormat() != QRhiTexture::RGBA8) {
          resolve_tex_ = rhi_->newTexture(QRhiTexture::RGBA8, QSize(tex_width, tex_height),
                                          1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
          resolve_tex_->create();
          resolve_rt_ = rhi_->newTextureRenderTarget({resolve_tex_});
          resolve_rpd_ = resolve_rt_->newCompatibleRenderPassDescriptor();
          resolve_rt_->setRenderPassDescriptor(resolve_rpd_);
          resolve_rt_->create();
        }
      }

      // Create back buffer (single — used for clip→main compositing)
      if (back_tex_ == nullptr) {
        back_tex_ = rhi_->newTexture(IntermediateFormat(), QSize(tex_width, tex_height),
                                     1, QRhiTexture::RenderTarget);
        back_tex_->create();
        back_rt_ = rhi_->newTextureRenderTarget({back_tex_});
        back_rpd_ = back_rt_->newCompatibleRenderPassDescriptor();
        back_rt_->setRenderPassDescriptor(back_rpd_);
        back_rt_->create();
      }

      // Draw frame
      paint();

      front_buffer_switcher = !front_buffer_switcher;

      emit ready();
    }
  }

  delete_ctx();

  wait_lock_.unlock();
}

QMutex* RenderThread::get_texture_mutex(int buffer_index) {
  return buffer_index ? &front_mutex2 : &front_mutex1;
}

int RenderThread::front_buffer_index() const {
  return front_buffer_switcher.load() ? 1 : 0;
}

const char* RenderThread::get_frame_data(int buffer_index) const {
  const QByteArray& buf = cpu_frame_[buffer_index];
  return buf.isEmpty() ? nullptr : buf.constData();
}

int RenderThread::get_frame_width(int buffer_index) const { return cpu_frame_w_[buffer_index]; }
int RenderThread::get_frame_height(int buffer_index) const { return cpu_frame_h_[buffer_index]; }

void RenderThread::paint() {
  int active_idx = front_buffer_switcher ? 0 : 1;

  QMutex& active_mutex = front_buffer_switcher ? front_mutex1 : front_mutex2;
  active_mutex.lock();

  QRhiCommandBuffer* cb = nullptr;
  if (rhi_->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) {
    qWarning() << "beginOffscreenFrame failed, skipping frame";
    active_mutex.unlock();
    texture_failed = true;
    return;
  }

  // Clear the main target (use non-PreserveColorContents RT so clear actually happens)
  {
    QColor clearColor(0, 0, 0, 0);
    cb->beginPass(front_rt_clear_[active_idx], clearColor, {1.0f, 0});
    cb->endPass();
  }

  // Set up compose_sequence() parameters
  ComposeSequenceParams params;
  params.viewer = nullptr;
  params.rhi = rhi_;
  params.cb = cb;
  params.seq = seq;
  params.video = true;
  params.texture_failed = false;
  params.wait_for_mutexes = true;
  params.playback_speed = playback_speed_;
  params.scrubbing = scrubbing_;

  params.sampler = sampler_;

  params.passthroughVert = passthroughVert_;
  params.passthroughFrag = passthroughFrag_;
  params.blendingFrag = blendingFrag_;
  params.premultiplyFrag = premultiplyFrag_;
  params.yuvFrag = yuvFrag_;
  params.resolveFrag = resolveFrag_;
  params.downscaleFrag = downscaleFrag_;
  params.yuv10LumaFrag = yuv10LumaFrag_;
  params.yuv10ChromaFrag = yuv10ChromaFrag_;

  params.main_tex = front_tex_[active_idx];
  params.main_target = front_rt_[active_idx];
  params.main_rpd = front_rpd_;

  params.backend_tex1 = back_tex_;
  params.backend_target1 = back_rt_;
  params.backend_rpd = back_rpd_;

  gizmos = seq->GetSelectedGizmo();
  params.gizmos = gizmos;

  amber::rendering::compose_sequence(params);
  gizmos = params.gizmos;  // nulled by compose_sequence if clip wasn't rendered

  // A 16-bit export reads the float target as it is. Everything else reads the 8-bit copy made here (with dither)
  const bool export16 = pixel_buffer != nullptr && pixel_buffer_16bit;
  QRhiTexture* readback_tex = front_tex_[active_idx];
  const QSize composite_size(tex_width, tex_height);
  QSize frame_size = composite_size;  // size of the frame that is read back
  if (pixel_buffer != nullptr && export_size_.isValid() && export_size_ != composite_size) {
    // The export is a different size from the composite: average down on the GPU, so the readback is output-sized
    ensure_export_target(export_size_, !export16);
    amber::rendering::downscale_export(params, front_tex_[active_idx], composite_size, export_rt_, export_rpd_,
                                       export_size_, !export16);
    readback_tex = export_tex_;
    frame_size = export_size_;
  } else if (resolve_tex_ != nullptr && !export16) {
    amber::rendering::resolve_to_8bit(params, front_tex_[active_idx], resolve_rt_, resolve_rpd_);
    readback_tex = resolve_tex_;
  }

  // 10-bit 4:2:0 export: the GPU writes the luma and chroma planes, and only those are read back. For 16-bit
  // exports readback_tex is already the RGB frame at the output size.
  const bool yuv_planes = pixel_buffer != nullptr && export_planes_valid_ && export16;
  QRhiReadbackResult readback_uv;
  if (yuv_planes) {
    ensure_yuv_targets(frame_size);
    amber::rendering::yuv10_luma(params, readback_tex, yuv_y_rt_, yuv_y_rpd_, export_planes_.msb_aligned);
    amber::rendering::yuv10_chroma(params, readback_tex, frame_size, yuv_uv_rt_, yuv_uv_rpd_,
                                   export_planes_.msb_aligned);
    readback_tex = yuv_y_tex_;
  }

  // CPU readback
  {
    QRhiReadbackResult readback;
    QRhiResourceUpdateBatch* u = rhi_->nextResourceUpdateBatch();
    u->readBackTexture(QRhiReadbackDescription(readback_tex), &readback);
    if (yuv_planes) {
      u->readBackTexture(QRhiReadbackDescription(yuv_uv_tex_), &readback_uv);
    }
    QColor clearColor(0, 0, 0, 0);
    cb->beginPass(front_rt_[active_idx], clearColor, {1.0f, 0});
    cb->endPass(u);

    rhi_->endOffscreenFrame();

    qDeleteAll(params.transientResources);
    params.transientResources.clear();

    // Drain deferred deletes from other threads (e.g. Clip::Close on main thread)
    drainDeferredDeletes();

    if (yuv_planes) {
      copy_yuv_planes(readback, readback_uv, export_planes_, frame_size);
    } else if (export16) {
      export_frame16_ = readback.data;
    } else {
      QByteArray& dst = cpu_frame_[active_idx];
      dst = readback.data;
      cpu_frame_w_[active_idx] = readback.pixelSize.width();
      cpu_frame_h_[active_idx] = readback.pixelSize.height();
    }
  }

  active_mutex.unlock();

  texture_failed = params.texture_failed;

  // readBackTexture() returns top-to-bottom data on all backends (both OpenGL
  // and Vulkan/Metal/D3D). No row reversal needed for export or save-frame.
  // Note: this differs from framebuffer readback (glReadPixels), which IS
  // bottom-to-top on OpenGL — but we read from a texture, not the framebuffer.

  // Save frame if requested
  if (!save_fn.isEmpty()) {
    if (texture_failed) {
      queued = true;
    } else {
      QByteArray& data = cpu_frame_[active_idx];
      if (!data.isEmpty()) {
        // Copy the QImage so it owns its pixels — save_fn is cleared below and
        // cpu_frame_ may be overwritten before the PNG/JPEG encoder finishes.
        QImage img =
            QImage(reinterpret_cast<const uchar*>(data.constData()), tex_width, tex_height, QImage::Format_RGBA8888)
                .copy();
        if (!img.save(save_fn)) {
          qWarning() << "Failed to save frame to" << save_fn;
          emit frame_save_failed(save_fn);
        }
      } else {
        qWarning() << "Failed to save frame to" << save_fn << "- frame buffer empty";
        emit frame_save_failed(save_fn);
      }
      save_fn = "";
    }
  }

  // Export pixel buffer if requested (the plane path has already written its destination)
  if (pixel_buffer != nullptr) {
    QByteArray& data = export16 ? export_frame16_ : cpu_frame_[active_idx];
    if (!data.isEmpty() && !yuv_planes) {
      const int bytes_per_pixel = export16 ? 8 : 4;  // half-float RGBA or 8-bit RGBA
      int copy_width = pixel_buffer_linesize == 0 ? frame_size.width() : pixel_buffer_linesize;
      int row_bytes = copy_width * bytes_per_pixel;
      int src_stride = frame_size.width() * bytes_per_pixel;
      if (row_bytes > src_stride) {
        // Per-row copy with padding zero-fill when destination has padding
        const char* src = data.constData();
        char* dst = static_cast<char*>(pixel_buffer);
        for (int y = 0; y < frame_size.height(); y++) {
          char* dst_row = dst + static_cast<qsizetype>(y) * row_bytes;
          memcpy(dst_row, src + static_cast<qsizetype>(y) * src_stride, src_stride);
          memset(dst_row + src_stride, 0, row_bytes - src_stride);
        }
      } else {
        int bytes = row_bytes * frame_size.height();
        memcpy(pixel_buffer, data.constData(), qMin(bytes, int(data.size())));
      }
    }
    pixel_buffer = nullptr;
    pixel_buffer_16bit = false;
  }
}

void RenderThread::start_render(Sequence* s,
                                int playback_speed,
                                const QString& save,
                                void* pixels,
                                int pixel_linesize,
                                int idivider,
                                bool scrubbing,
                                bool half_float_pixels,
                                int export_width,
                                int export_height,
                                const ExportPlanes* planes) {
  // Apply preview resolution divider for playback (not export)
  if (pixels == nullptr) {
    divider_ = qMax(1, idivider);
  } else {
    divider_ = 1;  // Always export at full resolution
  }

  // Export path (pixel_buffer != nullptr) and Save Frame path (save non-empty) need
  // the lock to prevent a lost-wakeup deadlock and to avoid racing paint() on save_fn.
  // Viewer/scrubbing refresh (both null) must NOT lock — the render thread holds
  // wait_lock_ during the entire paint() operation, so locking here would make
  // scrubbing synchronous (UI blocks until previous frame finishes rendering).
  bool needs_lock = (pixels != nullptr) || !save.isEmpty();
  if (needs_lock) {
    wait_lock_.lock();
  }

  seq = s;

  playback_speed_ = playback_speed;
  scrubbing_ = scrubbing;

  // stall any dependent actions
  texture_failed = true;

  // Refresh/retry calls (frame_update, paintGL did_texture_fail retry) pass save=""
  // and pixels=null — they must NOT clobber a pending Save Frame request that is
  // still waiting for a successful render, nor an in-flight export pixel buffer.
  // paint() clears save_fn / pixel_buffer itself once it has consumed them.
  if (!save.isEmpty()) {
    save_fn = save;
  }
  if (pixels != nullptr) {
    pixel_buffer = pixels;
    pixel_buffer_linesize = pixel_linesize;
    pixel_buffer_16bit = half_float_pixels;
    // Render at the output size, so the frame is not rendered at the sequence size and downscaled afterwards
    export_size_ = (export_width > 0 && export_height > 0) ? QSize(export_width, export_height) : QSize();
    export_planes_valid_ = planes != nullptr;
    if (planes != nullptr) export_planes_ = *planes;
  }

  queued = true;

  wait_cond_.wakeAll();

  if (needs_lock) {
    wait_lock_.unlock();
  }
}

bool RenderThread::did_texture_fail() { return texture_failed; }

bool RenderThread::rhi_failed() const { return rhi_failed_.load(); }

void RenderThread::cancel() {
  running = false;
  wait_cond_.wakeAll();
  wait();
}

void RenderThread::wait_until_paused() {
  if (wait_lock_.tryLock()) {
    wait_lock_.unlock();
    return;
  } else {
    wait_lock_.lock();
    wait_lock_.unlock();
  }
}

void RenderThread::delete_buffers() {
  for (int i = 0; i < 2; i++) {
    delete front_rt_clear_[i];
    front_rt_clear_[i] = nullptr;
    delete front_rt_[i];
    front_rt_[i] = nullptr;
    delete front_tex_[i];
    front_tex_[i] = nullptr;
  }
  delete front_clear_rpd_;
  front_clear_rpd_ = nullptr;
  delete front_rpd_;
  front_rpd_ = nullptr;
  delete back_rt_;
  back_rt_ = nullptr;
  delete back_tex_;
  back_tex_ = nullptr;
  delete back_rpd_;
  back_rpd_ = nullptr;
  delete resolve_rt_;
  resolve_rt_ = nullptr;
  delete resolve_tex_;
  resolve_tex_ = nullptr;
  delete resolve_rpd_;
  resolve_rpd_ = nullptr;
  delete_export_target();
  delete_yuv_targets();
}

void RenderThread::delete_export_target() {
  delete export_rt_;
  export_rt_ = nullptr;
  delete export_tex_;
  export_tex_ = nullptr;
  delete export_rpd_;
  export_rpd_ = nullptr;
  export_tex_size_ = QSize();
}

void RenderThread::ensure_export_target(QSize size, bool eight_bit) {
  if (export_tex_ != nullptr && export_tex_size_ == size && export_tex_8bit_ == eight_bit) return;
  delete_export_target();
  const QRhiTexture::Format format = eight_bit ? QRhiTexture::RGBA8 : IntermediateFormat();
  export_tex_ = rhi_->newTexture(format, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
  export_tex_->create();
  export_rt_ = rhi_->newTextureRenderTarget({export_tex_});
  export_rpd_ = export_rt_->newCompatibleRenderPassDescriptor();
  export_rt_->setRenderPassDescriptor(export_rpd_);
  export_rt_->create();
  export_tex_size_ = size;
  export_tex_8bit_ = eight_bit;
}

void RenderThread::delete_ctx() {
  drainDeferredDeletes();  // Clean up anything queued before shutdown
  delete_buffers();

  delete sampler_;
  sampler_ = nullptr;

  delete rhi_;
  rhi_ = nullptr;

  if (owns_fallback_surface_) {
    delete fallbackSurface_;
  }
  fallbackSurface_ = nullptr;
  owns_fallback_surface_ = false;
}

void RenderThread::delete_yuv_targets() {
  delete yuv_y_rt_;
  yuv_y_rt_ = nullptr;
  delete yuv_y_tex_;
  yuv_y_tex_ = nullptr;
  delete yuv_y_rpd_;
  yuv_y_rpd_ = nullptr;
  delete yuv_uv_rt_;
  yuv_uv_rt_ = nullptr;
  delete yuv_uv_tex_;
  yuv_uv_tex_ = nullptr;
  delete yuv_uv_rpd_;
  yuv_uv_rpd_ = nullptr;
  yuv_size_ = QSize();
}

// Luma is 16-bit per sample at the output size; chroma is 2x2 samples per 4 luma samples, two channels (Cb, Cr)
void RenderThread::ensure_yuv_targets(QSize luma_size) {
  if (yuv_y_tex_ != nullptr && yuv_size_ == luma_size) return;
  delete_yuv_targets();
  const QSize chroma_size(luma_size.width() / 2, luma_size.height() / 2);

  yuv_y_tex_ = rhi_->newTexture(QRhiTexture::R16, luma_size, 1,
                                QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
  yuv_y_tex_->create();
  yuv_y_rt_ = rhi_->newTextureRenderTarget({yuv_y_tex_});
  yuv_y_rpd_ = yuv_y_rt_->newCompatibleRenderPassDescriptor();
  yuv_y_rt_->setRenderPassDescriptor(yuv_y_rpd_);
  yuv_y_rt_->create();

  yuv_uv_tex_ = rhi_->newTexture(QRhiTexture::RG16, chroma_size, 1,
                                 QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
  yuv_uv_tex_->create();
  yuv_uv_rt_ = rhi_->newTextureRenderTarget({yuv_uv_tex_});
  yuv_uv_rpd_ = yuv_uv_rt_->newCompatibleRenderPassDescriptor();
  yuv_uv_rt_->setRenderPassDescriptor(yuv_uv_rpd_);
  yuv_uv_rt_->create();

  yuv_size_ = luma_size;
}

// Copies the read-back luma and chroma into the export planes. Rows are tight in the read-back, so only the
// destination stride needs care.
static void copy_yuv_planes(const QRhiReadbackResult& luma, const QRhiReadbackResult& chroma,
                            const ExportPlanes& dst, QSize size) {
  const int w = size.width();
  const int h = size.height();
  const int cw = w / 2;
  const int ch = h / 2;
  if (luma.data.size() < w * 2 * h || chroma.data.size() < cw * 4 * ch) {
    qWarning() << "Export planes: read-back is smaller than the frame";
    return;
  }
  for (int y = 0; y < h; y++) {
    memcpy(dst.y + y * dst.y_stride, luma.data.constData() + y * w * 2, w * 2);
  }
  for (int y = 0; y < ch; y++) {
    const char* src = chroma.data.constData() + y * cw * 4;
    quint8* u_row = dst.u + y * dst.u_stride;
    quint8* v_row = dst.v + y * dst.v_stride;
    for (int x = 0; x < cw; x++) {
      memcpy(u_row + x * dst.u_step, src + x * 4, 2);
      memcpy(v_row + x * dst.v_step, src + x * 4 + 2, 2);
    }
  }
}
