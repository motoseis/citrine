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

#include "colorwheel.h"

#include <QConicalGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QtMath>

ColorWheel::ColorWheel(QWidget* parent) : QWidget(parent), hue_(0.0), saturation_(0.0), dragging_(false) {
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize ColorWheel::sizeHint() const { return QSize(80, 80); }

void ColorWheel::set_color(const QColor& c) {
  // achromatic colours report a hue of -1, so keep the last hue
  if (c.hsvHueF() >= 0.0) hue_ = c.hsvHueF();
  saturation_ = qBound(0.0, c.hsvSaturationF(), 1.0);
  update();
}

QColor ColorWheel::color() const { return QColor::fromHsvF(hue_, saturation_, 1.0); }

double ColorWheel::radius() const { return qMax(4.0, qMin(width(), height()) / 2.0 - 4.0); }

QPointF ColorWheel::centre() const { return QPointF(width() / 2.0, height() / 2.0); }

void ColorWheel::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  const QPointF c = centre();
  const double r = radius();

  // hue ring: full saturation at the edge, neutral at the centre
  QConicalGradient hue(c, 0.0);
  for (int i = 0; i <= 12; i++) {
    double h = i / 12.0;
    hue.setColorAt(h, QColor::fromHsvF(h, 1.0, 1.0));
  }
  p.setPen(Qt::NoPen);
  p.setBrush(hue);
  p.drawEllipse(c, r, r);

  QRadialGradient fade(c, r);
  fade.setColorAt(0.0, QColor(255, 255, 255, 255));
  fade.setColorAt(1.0, QColor(255, 255, 255, 0));
  p.setBrush(fade);
  p.drawEllipse(c, r, r);

  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(QColor(0, 0, 0, 140), 1.0));
  p.drawEllipse(c, r, r);

  // current position
  const double angle = hue_ * 2.0 * M_PI;
  const QPointF pos(c.x() + r * saturation_ * qCos(angle), c.y() - r * saturation_ * qSin(angle));
  p.setPen(QPen(Qt::black, 1.0));
  p.setBrush(color());
  p.drawEllipse(pos, 4.5, 4.5);
  p.setPen(QPen(Qt::white, 1.0));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(pos, 5.5, 5.5);
}

void ColorWheel::set_from_point(const QPointF& p) {
  const QPointF c = centre();
  const double dx = p.x() - c.x();
  const double dy = c.y() - p.y();  // screen y points down, the wheel's angle is counter-clockwise
  double angle = qAtan2(dy, dx);
  if (angle < 0) angle += 2.0 * M_PI;
  hue_ = angle / (2.0 * M_PI);
  saturation_ = qMin(1.0, qSqrt(dx * dx + dy * dy) / radius());
  update();
}

void ColorWheel::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) return;
  dragging_ = true;
  set_from_point(event->position());
}

void ColorWheel::mouseMoveEvent(QMouseEvent* event) {
  if (dragging_) set_from_point(event->position());
}

void ColorWheel::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !dragging_) return;
  dragging_ = false;
  emit color_changed(color());
}

void ColorWheel::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) return;
  // double-click resets the wheel to neutral
  saturation_ = 0.0;
  update();
  emit color_changed(color());
}
