#include "colorfield.h"

#include <QColor>
#include <QHBoxLayout>
#include <QPushButton>

#include "panels/panels.h"
#include "panels/viewer.h"
#include "ui/colorbutton.h"
#include "ui/colorwheel.h"
#include "ui/icons.h"
#include "ui/viewerwidget.h"

ColorField::ColorField(EffectRow* parent, const QString& id) :
  EffectField(parent, id, EFFECT_FIELD_COLOR)
{}

void ColorField::SetWheel(bool wheel)
{
  wheel_ = wheel;
}

QColor ColorField::GetColorAt(double timecode)
{
  return GetValueAt(timecode).value<QColor>();
}

void ColorField::disconnectPick() {
  QObject::disconnect(pick_connection_);
  QObject::disconnect(cancel_connection_);
}

QWidget *ColorField::CreateWidget(QWidget *existing)
{
  if (wheel_) {
    QWidget* wheel_wrapper;
    ColorWheel* wheel;
    if (existing != nullptr) {
      wheel_wrapper = existing;
      wheel = existing->findChild<ColorWheel*>();
    } else {
      wheel_wrapper = new QWidget();
      wheel_wrapper->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
      QHBoxLayout* wheel_layout = new QHBoxLayout(wheel_wrapper);
      wheel_layout->setContentsMargins(0, 0, 0, 0);
      wheel = new ColorWheel();
      wheel_layout->addWidget(wheel);
    }
    if (wheel != nullptr) connect(wheel, &ColorWheel::color_changed, this, &ColorField::UpdateFromWidget);
    connect(this, &EffectField::EnabledChanged, wheel_wrapper, &QWidget::setEnabled);
    return wheel_wrapper;
  }

  ColorButton* cb;
  QWidget* wrapper;

  if (existing != nullptr) {
    wrapper = existing;
    cb = existing->findChild<ColorButton*>();
  } else {
    wrapper = new QWidget();
    wrapper->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);

    QHBoxLayout* layout = new QHBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    cb = new ColorButton();
    layout->addWidget(cb);

    QPushButton* eyedropper = new QPushButton();
    eyedropper->setIcon(amber::icon::CreateIconFromSVG(":/icons/eyedropper.svg", false));
    eyedropper->setIconSize(QSize(10, 10));
    eyedropper->setFixedSize(16, 16);
    eyedropper->setFlat(true);
    eyedropper->setToolTip(tr("Pick color from viewer"));
    layout->addWidget(eyedropper);

    connect(eyedropper, &QPushButton::clicked, this, [this, cb]() {
      if (panel_sequence_viewer == nullptr || panel_sequence_viewer->viewer_widget == nullptr) {
        return;
      }
      ViewerWidget* vw = panel_sequence_viewer->viewer_widget;

      // Disconnect any previous pick (if user clicked eyedropper twice)
      disconnectPick();

      vw->startColorPick();

      pick_connection_ = connect(vw, &ViewerWidget::colorPicked, this, [this, cb](const QColor& c) {
        disconnectPick();
        cb->set_color(c);
        emit cb->color_changed(c);
      });

      cancel_connection_ = connect(vw, &ViewerWidget::colorPickCancelled, this, [this]() {
        disconnectPick();
      });
    });
  }

  connect(cb, &ColorButton::color_changed, this, &ColorField::UpdateFromWidget);
  connect(this, &EffectField::EnabledChanged, wrapper, &QWidget::setEnabled);

  return wrapper;
}

void ColorField::UpdateWidgetValue(QWidget *widget, double timecode)
{
  if (wheel_) {
    ColorWheel* wheel = widget->findChild<ColorWheel*>();
    if (wheel != nullptr) wheel->set_color(GetColorAt(timecode));
    return;
  }

  ColorButton* cb = widget->findChild<ColorButton*>();
  if (cb != nullptr) {
    cb->set_color(GetColorAt(timecode));
  }
}

QVariant ColorField::ConvertStringToValue(const QString &s)
{
  return QColor(s);
}

QString ColorField::ConvertValueToString(const QVariant &v)
{
  return v.value<QColor>().name();
}

void ColorField::UpdateFromWidget(const QColor& c)
{
  KeyframeDataChange* kdc = new KeyframeDataChange(this);

  SetValueAt(Now(), c);

  kdc->SetNewKeyframes();
  kdc->setText(QObject::tr("Change Color"));
  amber::UndoStack.push(kdc);
}
