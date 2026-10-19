/***

    Amber - Non-Linear Video Editor
    Copyright (C) 2026  Amber Team

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

#include "drawlineeffect.h"
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QtMath>
#include "engine/clip.h"
#include "engine/sequence.h"

namespace {
double point_distance(const QPointF& a, const QPointF& b) {
  double dx = b.x() - a.x();
  double dy = b.y() - a.y();
  return std::sqrt(dx * dx + dy * dy);
}
}  // namespace

DrawLineEffect::DrawLineEffect(Clip* c, const EffectMeta* em) : Effect(c, em) {
  EffectRow* x_row = new EffectRow(this, tr("Line X (%)"));
  route_x = new DoubleField(x_row, "route_x");

  EffectRow* y_row = new EffectRow(this, tr("Line Y (%)"));
  route_y = new DoubleField(y_row, "route_y");

  EffectRow* style_row = new EffectRow(this, tr("Line Thickness"));
  thickness_val = new DoubleField(style_row, "thickness");
  route_x->SetDefault(1);
  route_x->SetMaximum(100);
  route_x->SetMinimum(0);

  EffectRow* color_row = new EffectRow(this, tr("Color"), true, false);
  color_fld = new ColorField(color_row, "color");
  color_fld->SetColumnSpan(2);
  color_fld->SetValueAt(0, QColor(Qt::red));

  EffectRow* smooth_sz = new EffectRow(this, tr("Smoothing number of points"), true, false);
  smooth_fld = new DoubleField(smooth_sz, "smoothsz");
  smooth_fld->SetDefault(1);
  smooth_fld->SetMaximum(31);
  smooth_fld->SetMinimum(1);

  EffectRow* arrow_row = new EffectRow(this, tr("Arrow Size"), true, false);
  arrow_fld = new DoubleField(arrow_row, "arrowsz");
  arrow_fld->SetDefault(1);
  arrow_fld->SetMaximum(100);
  arrow_fld->SetMinimum(0);

  EffectRow* pcent_row = new EffectRow(this, tr("Pcent of frame to draw"), true, false);
  pcent_fld = new DoubleField(pcent_row, "pcent");
  pcent_fld->SetDefault(80);
  pcent_fld->SetMaximum(100);
  pcent_fld->SetMinimum(50);

  EffectRow* kfdone_row = new EffectRow(this, tr("Flag when keyframes are complete"), true, false);
  kfdone_fld = new BoolField(kfdone_row, "KeyFramesDone");
//  kfdone_fld->SetColumnSpan(2);

  route_y->SetDefault(50);
  route_y->SetMaximum(100);
  route_y->SetMinimum(0);

  thickness_val->SetDefault(6);
  thickness_val->SetMinimum(0);
  thickness_val->SetMaximum(15);

  SetFlags(Effect::SuperimposeFlag);
}

DrawLineEffect::~DrawLineEffect() {}

bool DrawLineEffect::AlwaysUpdate() { return true; }

void DrawLineEffect::rebuild_path_cache(double reference_timecode, double reference_frame) {
  int width = img.width();
  int height = img.height();

  double clip_start = parent_clip->timeline_in();
  double clip_end = parent_clip->timeline_out();
  double fps = amber::ActiveSequence->frame_rate;
  if (fps <= 0.0) fps = 29.97;
  double frame_duration = 1.0 / fps;

  int frame_count = int(clip_end - clip_start) + 1;
  if (frame_count <= 0) {
    path_cache_valid_ = false;
    return;
  }
  QVector<QPointF> raw_points;
  raw_points.reserve(frame_count);

  for (int i = 0; i < frame_count; ++i) {
    double this_frame = clip_start + i;
    double target_t = reference_timecode + (this_frame - reference_frame) * frame_duration;

    double kf_x = route_x->GetDoubleAt(target_t) * 0.01 * width;
    double kf_y = route_y->GetDoubleAt(target_t) * 0.01 * height;
    raw_points.append(QPointF(kf_x, kf_y));
  }

  int nsmooth = smooth_fld->GetDoubleAt(reference_timecode);
  cached_points_ = smoothTrajectoryFilter(raw_points, nsmooth, 0.5);

  // Build cumulative arc length at each point
  cached_cumulative_len_.clear();
  cached_cumulative_len_.reserve(frame_count);
  cached_cumulative_len_.append(0.0);
  for (int i = 1; i < cached_points_.size(); ++i) {
    double step = point_distance(cached_points_[i - 1], cached_points_[i]);
    cached_cumulative_len_.append(cached_cumulative_len_[i - 1] + step);
  }
  cached_total_length_ = cached_cumulative_len_.isEmpty() ? 0.0 : cached_cumulative_len_.last();

  cached_frame_count_ = frame_count;
  path_cache_valid_ = true;
}

QPointF DrawLineEffect::point_at_length_fraction(double t) {
  if (cached_points_.isEmpty()) return QPointF();
  if (cached_points_.size() == 1 || cached_total_length_ <= 0.0) return cached_points_.first();

  double target_length = qBound(0.0, t, 1.0) * cached_total_length_;

  // Find the segment containing target_length
  int seg = 0;
  while (seg < cached_cumulative_len_.size() - 2 && cached_cumulative_len_[seg + 1] < target_length) {
    seg++;
  }

  double seg_start_len = cached_cumulative_len_[seg];
  double seg_len = cached_cumulative_len_[seg + 1] - seg_start_len;
  double local_t = (seg_len > 0.0) ? (target_length - seg_start_len) / seg_len : 0.0;

  return cached_points_[seg] + local_t * (cached_points_[seg + 1] - cached_points_[seg]);
}

void DrawLineEffect::draw_route_line(const QVector<QPointF>& points, double timecode) {
  int width = img.width();
  int height = img.height();

  QPainterPath drawing_path;
  drawing_path.moveTo(points.first());
  for (int i = 1; i < points.size(); ++i) {
    drawing_path.lineTo(points[i]);
  }
  double thick = thickness_val->GetDoubleAt(timecode);
  double arrow_len = arrow_fld->GetDoubleAt(timecode);

  QPainter painter(&img);
  painter.setRenderHint(QPainter::Antialiasing);
  QColor bkg = color_fld->GetColorAt(timecode);
  //  QPen line_pen(Qt::red, thick, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  QPen line_pen(bkg, thick, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.setPen(line_pen);
  painter.drawPath(drawing_path);

  if (points.size() > 1) {
    QPointF tip_point = points.last();
    double heading_degrees = -drawing_path.angleAtPercent(1.0);
    double heading_radians = heading_degrees * (M_PI / 180.0);
    //    double arrow_len = thick * 3.0;

    QPointF tip_front = tip_point;
    QPointF tip_back_left(tip_point.x() - arrow_len * qCos(heading_radians - M_PI / 6),
                          tip_point.y() - arrow_len * qSin(heading_radians - M_PI / 6));
    QPointF tip_back_right(tip_point.x() - arrow_len * qCos(heading_radians + M_PI / 6),
                           tip_point.y() - arrow_len * qSin(heading_radians + M_PI / 6));

    QPainterPath arrowhead_path;
    arrowhead_path.moveTo(tip_front);
    arrowhead_path.lineTo(tip_back_left);
    arrowhead_path.lineTo(tip_back_right);
    arrowhead_path.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QBrush(bkg));
    painter.drawPath(arrowhead_path);
  }
}

//======================================================
void DrawLineEffect::redraw(double timecode) {
  int width = img.width();
  int height = img.height();
  if (width <= 0 || height <= 0) return;
  img.fill(Qt::transparent);
  int nsmooth = smooth_fld->GetDoubleAt(timecode);

  double current_frame = amber::ActiveSequence->playhead;
  double clip_start = parent_clip->timeline_in();
  double clip_end = parent_clip->timeline_out();

  if (current_frame < clip_start) return;

  double fps = amber::ActiveSequence->frame_rate;
  if (fps <= 0.0) fps = 29.97;
  double frame_duration = 1.0 / fps;

  int total_history_steps = int(current_frame - clip_start);
  QVector<QPointF> drawing_points;
  bool kFDone = kfdone_fld->GetBoolAt(timecode);

  if (kFDone) {
    if (!path_cache_valid_ || cached_frame_count_ != int(clip_end - clip_start) + 1) {
      rebuild_path_cache(timecode, current_frame);
    }
    int animated_frames = 0.01 * pcent_fld->GetDoubleAt(timecode) * cached_frame_count_;
    double frame_in_clip = current_frame - clip_start;
    double t = (animated_frames > 1) ? qBound(0.0, frame_in_clip / (animated_frames - 1), 1.0) : 1.0;

    double target_length = t * cached_total_length_;
    for (int i = 0; i < cached_points_.size(); ++i) {
      if (cached_cumulative_len_[i] <= target_length) {
        drawing_points.append(cached_points_[i]);
      } else {
        break;
      }
    }
    drawing_points.append(point_at_length_fraction(t));
    this->draw_route_line(drawing_points, timecode);
  } else {
    for (int i = 0; i <= total_history_steps; i++) {
      // March forward step-by-step using your verified time calculation formula
      double target_t = timecode - (total_history_steps * frame_duration) + (i * frame_duration);

      double kf_x = route_x->GetDoubleAt(target_t) * 0.01 * width;
      double kf_y = route_y->GetDoubleAt(target_t) * 0.01 * height;

      QPointF pt(kf_x, kf_y);
      drawing_points.append(pt);
    }
    if (drawing_points.isEmpty()) return;
    if (drawing_points.size() == 1) {
      drawing_points.append(drawing_points.first() + QPointF(1.0, 0.0));
    }

    int nsmooth = smooth_fld->GetDoubleAt(timecode);
    QVector<QPointF> smoothed = this->smoothTrajectoryFilter(drawing_points, nsmooth, 0.5);
    this->draw_route_line(smoothed, timecode);
    path_cache_valid_ = false;
  }
}

/**
 * @brief Smooths corners of a time-series trajectory while keeping straight lines intact.
 * @param inputPoints 30-60 Hz sampled curve (up to ~300 points).
 * @param windowSize Number of points to smooth over (e.g., 15 for ~0.25-0.5s of motion).
 * @param curvatureThreshold Threshold for corner detection.
 */
QVector<QPointF> DrawLineEffect::smoothTrajectoryFilter(const QVector<QPointF>& inputPoints, int windowSize,
                                                        double curvatureThreshold) {
  int n = inputPoints.size();
  if (n < 3 || windowSize < 3) {
    return inputPoints;
  }

  if (windowSize % 2 == 0) {
    windowSize++;  // Force odd window size
  }
  int halfWindow = windowSize / 2;
  QVector<QPointF> outputPoints = inputPoints;

  // Precompute Gaussian weights for the smoothing window to ensure perfect blending
  QVector<double> weights(windowSize);
  double sigma = halfWindow / 2.0;
  double weightSum = 0.0;
  for (int w = -halfWindow; w <= halfWindow; ++w) {
    double wIdx = w;
    double g = std::exp(-(wIdx * wIdx) / (2.0 * sigma * sigma));
    weights[w + halfWindow] = g;
    weightSum += g;
  }

  // Step 1: Compute second derivative magnitude (acceleration proxy / curvature)
  QVector<double> curvatures(n, 0.0);
  for (int i = 1; i < n - 1; ++i) {
    double d2x = inputPoints[i - 1].x() - 2.0 * inputPoints[i].x() + inputPoints[i + 1].x();
    double d2y = inputPoints[i - 1].y() - 2.0 * inputPoints[i].y() + inputPoints[i + 1].y();
    curvatures[i] = std::sqrt(d2x * d2x + d2y * d2y);
  }

  // Step 2: Smooth only regions close to sharp direction changes
  for (int i = halfWindow; i < n - halfWindow; ++i) {
    bool isNearCorner = false;

    // Look ahead/behind within the window radius to find if a corner is active
    for (int w = -halfWindow; w <= halfWindow; ++w) {
      if (curvatures[i + w] > curvatureThreshold) {
        isNearCorner = true;
        break;
      }
    }

    if (isNearCorner) {
      double sumX = 0.0;
      double sumY = 0.0;
      double currentWindowWeightSum = 0.0;

      for (int w = -halfWindow; w <= halfWindow; ++w) {
        const QPointF& p = inputPoints[i + w];
        double wFactor = weights[w + halfWindow];

        sumX += p.x() * wFactor;
        sumY += p.y() * wFactor;
        currentWindowWeightSum += wFactor;
      }

      outputPoints[i] = QPointF(sumX / currentWindowWeightSum, sumY / currentWindowWeightSum);
    }
  }

  return outputPoints;
}
