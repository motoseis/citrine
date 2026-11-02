#include "fontfield.h"

#include <QFontDatabase>
#include <QDebug>

#include "ui/comboboxex.h"

// NOTE/TODO: This shares a lot of similarity with ComboField, and could probably be a derived class of it

FontField::FontField(EffectRow* parent, const QString &id) :
  EffectField(parent, id, EFFECT_FIELD_FONT)
{
  font_list = QFontDatabase::families();

  SetValueAt(0, font_list.first());
}

QString FontField::GetFontAt(double timecode)
{
  return GetValueAt(timecode).toString();
}

QWidget *FontField::CreateWidget(QWidget *existing)
{
  ComboBoxEx* fcb = new ComboBoxEx();

  if (existing == nullptr) {

    fcb = new ComboBoxEx();

    fcb->setScrollingEnabled(false);
    fcb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    fcb->setMinimumContentsLength(12);

    fcb->addItems(font_list);

  } else {

    fcb = static_cast<ComboBoxEx*>(existing);

  }

  connect(fcb, &QComboBox::currentTextChanged, this, &FontField::UpdateFromWidget);
  connect(this, &EffectField::EnabledChanged, fcb, &QWidget::setEnabled);

  return fcb;
}

void FontField::UpdateWidgetValue(QWidget *widget, double timecode)
{
  QVariant data = GetValueAt(timecode);

  ComboBoxEx* cb = static_cast<ComboBoxEx*>(widget);

  // A saved font that isn't installed here (e.g. the project came from another machine) is added to the list,
  // so it stays selected instead of logging a warning on every refresh
  if (!font_list.contains(data.toString())) {
    font_list.append(data.toString());
  }
  for (int i = cb->count(); i < font_list.size(); i++) {
    cb->addItem(font_list.at(i));
  }

  cb->blockSignals(true);
  cb->setCurrentIndex(font_list.indexOf(data.toString()));
  cb->blockSignals(false);
}

void FontField::UpdateFromWidget(const QString& s)
{
  KeyframeDataChange* kdc = new KeyframeDataChange(this);

  SetValueAt(Now(), s);

  kdc->SetNewKeyframes();
  kdc->setText(QObject::tr("Change Font"));
  amber::UndoStack.push(kdc);
}
