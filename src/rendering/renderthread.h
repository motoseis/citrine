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

#ifndef RENDERTHREAD_H
#define RENDERTHREAD_H

#include <atomic>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QOffscreenSurface>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include "engine/sequence.h"
#include "effects/effect.h"

class RenderThread : public QThread {
  Q_OBJECT
public:
  RenderThread();
  ~RenderThread() override;
  void run() override;

  // Pre-set a fallback surface for the OpenGL backend.  Must be created on
  // the GUI thread (via QRhiGles2InitParams::newFallbackSurface()) and passed
  // here BEFORE start().  RenderThread does NOT take ownership — the caller
  // must ensure the surface outlives the RenderThread and delete it on the
  // GUI thread after cancel()/wait().
  void setGlFallbackSurface(QOffscreenSurface* surface);

  QMutex* get_texture_mutex(int buffer_index);

  // CPU bridge: pixel data read back after compositing
  const char* get_frame_data(int buffer_index) const;
  // Size of the pixels held by that buffer; read under get_texture_mutex(buffer_index), like the data
  int get_frame_width(int buffer_index) const;
  int get_frame_height(int buffer_index) const;

  Effect* gizmos{nullptr};
  void paint();
  // Returns the current front buffer index (snapshot of the atomic switcher).
  int front_buffer_index() const;

  void start_render(Sequence* s,
                    int playback_speed,
                    const QString& save = nullptr,
                    void* pixels = nullptr,
                    int pixel_linesize = 0,
                    int idivider = 0,
                    bool scrubbing = false,
                    bool half_float_pixels = false,
                    int export_width = 0,
                    int export_height = 0);
  bool did_texture_fail();
  // True once QRhi creation has failed. ready() is still emitted then, so a waiting ExportThread wakes up.
  bool rhi_failed() const;
  void cancel();
  void wait_until_paused();

  // Queue a QRhi resource for deferred deletion on the render thread.
  // Safe to call from any thread.
  static void DeferRhiResourceDeletion(QRhiResource* res);
  static void DeferRhiResourceDeletion(const QVector<QRhiResource*>& resources);

  // Format for GPU compositing targets: RGBA16F when the backend supports it, RGBA8 otherwise
  static QRhiTexture::Format IntermediateFormat();

public slots:
  void delete_ctx();
signals:
  void ready();
  void frame_save_failed(const QString& path);

private:
  static QMutex deferred_delete_mutex_;
  static QVector<QRhiResource*> deferred_delete_queue_;
  void drainDeferredDeletes();

  void delete_buffers();

  // RHI resources
  QRhi* rhi_{nullptr};
  QOffscreenSurface* fallbackSurface_{nullptr};
  bool owns_fallback_surface_{false};  // true when created internally (legacy path)

  // Core shaders loaded from QRC .qsb files
  QShader passthroughVert_;
  QShader passthroughFrag_;
  QShader blendingFrag_;
  QShader premultiplyFrag_;
  QShader yuvFrag_;
  QShader resolveFrag_;
  QShader downscaleFrag_;

  QRhiSampler* sampler_{nullptr};

  // Main compositing target (double-buffered)
  QRhiTexture* front_tex_[2] = {};
  QRhiTextureRenderTarget* front_rt_[2] = {};        // PreserveColorContents (for multi-clip compositing)
  QRhiTextureRenderTarget* front_rt_clear_[2] = {};  // No preserve (for initial clear)
  QRhiRenderPassDescriptor* front_rpd_{nullptr};
  QRhiRenderPassDescriptor* front_clear_rpd_{nullptr};

  QMutex front_mutex1;
  QMutex front_mutex2;
  std::atomic<bool> front_buffer_switcher;

  // Backend target (single — used for clip→main compositing)
  QRhiTexture* back_tex_{nullptr};
  QRhiTextureRenderTarget* back_rt_{nullptr};
  QRhiRenderPassDescriptor* back_rpd_{nullptr};

  // 8-bit copy of the final frame for the CPU readback (only used with RGBA16F intermediates)
  QRhiTexture* resolve_tex_{nullptr};
  QRhiTextureRenderTarget* resolve_rt_{nullptr};
  QRhiRenderPassDescriptor* resolve_rpd_{nullptr};

  // Export output target: the composite is averaged down to the export size on the GPU before readback.
  // Sized to the export and recreated when the export size or the output format changes.
  QRhiTexture* export_tex_{nullptr};
  QRhiTextureRenderTarget* export_rt_{nullptr};
  QRhiRenderPassDescriptor* export_rpd_{nullptr};
  QSize export_tex_size_;
  bool export_tex_8bit_{false};
  void ensure_export_target(QSize size, bool eight_bit);
  void delete_export_target();

  QWaitCondition wait_cond_;
  QMutex wait_lock_;

  QWaitCondition main_thread_wait_cond_;
  QMutex main_thread_lock_;

  Sequence* seq{nullptr};
  int playback_speed_;
  int tex_width{-1};
  int tex_height{-1};
  int divider_{1};
  QSize export_size_;  // output size of the export in progress; invalid when not exporting
  QAtomicInt queued;
  bool texture_failed{false};
  bool scrubbing_{false};
  bool running{true};
  std::atomic<bool> rhi_failed_{false};
  QString save_fn;
  void* pixel_buffer{nullptr};
  int pixel_buffer_linesize{0};
  // Export buffer holds half-float RGBA (8 bytes per pixel) instead of 8-bit RGBA
  bool pixel_buffer_16bit{false};
  QByteArray export_frame16_;  // readback for 16-bit exports; kept apart from cpu_frame_ so the viewer keeps 8-bit

  // CPU bridge: double-buffered pixel readback
  QByteArray cpu_frame_[2];
  int cpu_frame_w_[2]{0, 0}; // size of cpu_frame_[i], written with it under its buffer mutex
  int cpu_frame_h_[2]{0, 0};
};

#endif // RENDERTHREAD_H
