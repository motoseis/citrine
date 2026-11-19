/***

    Citrine - Non-Linear Video Editor
    Copyright (C) 2026 Citrine Team

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

#include "trackheaderwidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QVector>

#include "engine/sequence.h"
#include "global/global.h"
#include "panels/panels.h"
#include "panels/timeline.h"
#include "ui/timelinewidget.h"

namespace {

// Tracks in the same order and range the timeline draws them: video -1 down to video_count, audio 0 to audio_count
QVector<int> track_list(int video_count, int audio_count) {
  QVector<int> tracks;
  for (int t = -1; t >= video_count; --t) tracks.append(t);
  for (int t = 0; t <= audio_count; ++t) tracks.append(t);
  return tracks;
}

// The toggle button at the right of a track row: mute for audio tracks, visibility for video tracks
QRect toggle_button_rect(int row_top, int row_height) {
  const int size = 18;
  return QRect(TrackHeaderWidget::kWidth - size - 6, row_top + (row_height - size) / 2, size, size);
}

// Eye icon for video visibility; a slash is added when the track is hidden
void draw_eye(QPainter& p, const QRect& button, bool hidden) {
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(Qt::white, 1.2));
  p.drawEllipse(QRectF(button.left() + 2, button.center().y() - 4, button.width() - 4, 8));
  p.setPen(Qt::NoPen);
  p.setBrush(Qt::white);
  p.drawEllipse(QPointF(button.center()), 2.0, 2.0);
  if (hidden) {
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::white, 1.5));
    p.drawLine(button.left() + 3, button.bottom() - 3, button.right() - 3, button.top() + 3);
  }
}

}  // namespace

TrackHeaderWidget::TrackHeaderWidget(TimelineWidget* timeline, QWidget* parent)
    : QWidget(parent), timeline_(timeline) {
  setFixedWidth(kWidth);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void TrackHeaderWidget::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), QColor(45, 45, 45));

  // separator between the track headers and the sequence (timeline) panel
  p.setPen(QColor(150, 150, 150));
  p.drawLine(width() - 1, 0, width() - 1, height());

  if (amber::ActiveSequence == nullptr || timeline_ == nullptr || panel_timeline == nullptr) return;

  int video_count = 0;
  int audio_count = 0;
  amber::ActiveSequence->getTrackLimits(&video_count, &audio_count);

  p.setRenderHint(QPainter::Antialiasing, true);
  for (int track : track_list(video_count, audio_count)) {
    const int top = timeline_->trackScreenY(track);
    const int height = panel_timeline->GetTrackHeight(track);
    if (top + height < 0 || top > this->height()) continue;

    // row label: V1, V2... for video tracks and A1, A2... for audio tracks
    p.setPen(QColor(220, 220, 220));
    const QString label = (track < 0) ? QStringLiteral("V%1").arg(-track) : QStringLiteral("A%1").arg(track + 1);
    p.drawText(QRect(6, top, kWidth - 30, height), Qt::AlignVCenter | Qt::AlignLeft, label);

    if (track < 0) {
      const bool hidden = amber::ActiveSequence->hidden_video_tracks.contains(track);
      const QRect button = toggle_button_rect(top, height);
      p.setPen(Qt::NoPen);
      p.setBrush(hidden ? QColor(200, 70, 70) : QColor(95, 95, 95));
      p.drawRoundedRect(button, 3, 3);
      draw_eye(p, button, hidden);
    } else {
      const bool muted = amber::ActiveSequence->muted_audio_tracks.contains(track);
      const QRect button = toggle_button_rect(top, height);
      p.setPen(Qt::NoPen);
      p.setBrush(muted ? QColor(200, 70, 70) : QColor(95, 95, 95));
      p.drawRoundedRect(button, 3, 3);
      p.setPen(Qt::white);
      p.drawText(button, Qt::AlignCenter, QStringLiteral("M"));
    }
  }
}

void TrackHeaderWidget::mousePressEvent(QMouseEvent* event) {
  if (amber::ActiveSequence == nullptr || timeline_ == nullptr || panel_timeline == nullptr) return;

  const QPoint pos = event->position().toPoint();

  int video_count = 0;
  int audio_count = 0;
  amber::ActiveSequence->getTrackLimits(&video_count, &audio_count);

  const int track = timeline_->trackAtScreenY(pos.y());
  // valid video tracks are -1 down to video_count; valid audio tracks are 0 up to audio_count
  if (track < 0 && track < video_count) return;
  if (track > audio_count) return;

  const int top = timeline_->trackScreenY(track);
  const int height = panel_timeline->GetTrackHeight(track);
  if (!toggle_button_rect(top, height).contains(pos)) return;

  if (track < 0) {
    panel_timeline->toggle_video_track_hidden(track);
  } else {
    panel_timeline->toggle_audio_track_mute(track);
  }
}
