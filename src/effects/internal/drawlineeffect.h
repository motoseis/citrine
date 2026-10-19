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
#ifndef DRAWLINEEFFECT_H
#define DRAWLINEEFFECT_H

#include <QPointF>
#include <QVector>
#include "effects/effect.h"

class EffectGizmo;

class DrawLineEffect : public Effect {
  Q_OBJECT
 public:
  DrawLineEffect(Clip* c, const EffectMeta* em);
  virtual ~DrawLineEffect();

  void redraw(double timecode) override;

 protected:
  bool AlwaysUpdate() override;

 public:
 private:
  DoubleField* route_x;
  DoubleField* route_y;
  DoubleField* thickness_val;
  DoubleField* smooth_fld;
  DoubleField* pcent_fld;
  DoubleField* arrow_fld;
  ColorField* color_fld;
  BoolField* kfdone_fld;

  void draw_route_line(const QVector<QPointF>& points, double timecode);
  void rebuild_path_cache(double reference_timecode, double reference_frame);
  QPointF point_at_length_fraction(double t);
  QVector<QPointF> smoothTrajectoryFilter(const QVector<QPointF>& inputPoints, int windowSize = 15,
                                          double curvatureThreshold = 0.5);

  bool path_cache_valid_ = false;
  QVector<QPointF> cached_points_;         // one point per frame, raw keyframe positions
  QVector<double> cached_cumulative_len_;  // cumulative arc length up to cached_points_[i]

  double cached_total_length_ = 0.0;
  int cached_frame_count_ = 0;  // how many points/frames the cache above covers
};

#endif  // DRAWLINEEFFECT_H
