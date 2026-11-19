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

#ifndef TRACKHEADERWIDGET_H
#define TRACKHEADERWIDGET_H

#include <QWidget>

class TimelineWidget;

// A column to the left of the timeline with one row per track, aligned with the timeline's track rows.
// Audio tracks have a mute button.
class TrackHeaderWidget : public QWidget {
  Q_OBJECT
public:
  explicit TrackHeaderWidget(TimelineWidget* timeline, QWidget* parent = nullptr);

  static constexpr int kWidth = 64;

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;

private:
  TimelineWidget* timeline_;
};

#endif  // TRACKHEADERWIDGET_H
