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

#ifndef COLORWHEEL_H
#define COLORWHEEL_H

#include <QColor>
#include <QWidget>

// A colour wheel for choosing a tint: the angle picks the hue and the distance from the centre picks the
// saturation. The centre is neutral (white). Brightness is not part of the wheel; the master sliders handle it.
class ColorWheel : public QWidget {
  Q_OBJECT
public:
  explicit ColorWheel(QWidget* parent = nullptr);

  // Sets the wheel's tint without emitting color_changed
  void set_color(const QColor& c);
  QColor color() const;

signals:
  // Emitted when the user finishes changing the tint
  void color_changed(const QColor& c);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  QSize sizeHint() const override;

private:
  double radius() const;
  QPointF centre() const;
  void set_from_point(const QPointF& p);

  double hue_;         // 0..1
  double saturation_;  // 0..1
  bool dragging_;
};

#endif  // COLORWHEEL_H
