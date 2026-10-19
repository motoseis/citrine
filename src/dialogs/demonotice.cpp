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

#include "demonotice.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScreen>
#include <QSvgRenderer>

#include "global/config.h"

DemoNotice::DemoNotice(QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("Welcome to Citrine!"));
  setMinimumWidth(520);
  setSizeGripEnabled(true);

  QVBoxLayout* vlayout = new QVBoxLayout(this);

  QHBoxLayout* layout = new QHBoxLayout();
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(20);

  QLabel* icon = new QLabel(this);
  int logo_size = 128;
  qreal dpr = 1.0;
  if (QScreen* screen = QGuiApplication::primaryScreen()) {
    logo_size = qMin(screen->availableSize().width(), screen->availableSize().height()) / 6;
    if (logo_size < 128) logo_size = 128;
    dpr = screen->devicePixelRatio();
  }

  // 1. Corrected resource path syntax (Added the missing forward slash)
  QPixmap originalPix(":/icons/citrine-logo.png");

  // Graceful fallback check we discussed earlier
  if (originalPix.isNull()) {
    qWarning() << "⚠️ Error: Could not find resource image at ':/icons/citrine-logo.png'";
  } else {
    // 2. Compute physical target size matching your DPI calculations
    int target_physical_size = logo_size * dpr;

    // 3. Smoothly scale the image pixels up/down to match physical density
    QPixmap scaledPix =
        originalPix.scaled(target_physical_size, target_physical_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // 4. Set the device pixel ratio so Qt draws it crisp at 'logo_size' boundary
    scaledPix.setDevicePixelRatio(dpr);

    icon->setPixmap(scaledPix);
  }
  icon->setAlignment(Qt::AlignCenter);
  layout->addWidget(icon);

  QLabel* text = new QLabel(
      "<html><head/><body>"
      "<p><span style=\" font-size:14pt;\">" +
          tr("Welcome to Citrine!") + "</span></p><p>" +
          tr("Citrine is a fork of Amber v1.79 which was a fork of Olive 0.1 and ported to Qt 6 and modern FFmpeg. "
             "The original codebase was considered alpha-quality, but it has proven "
             "to be quite reliable in practice. Many bugs from the original code have "
             "been fixed along the way.") +
          "</p><p>" +
          tr("The original struck a rare balance between power and simplicity: "
             "a video editor where every feature you need is right where you expect it. ") +
          "</p><p>" + "</p></body></html>",
      this);
  text->setWordWrap(true);
  layout->addWidget(text);

  vlayout->addLayout(layout);

  dont_show_again_ = new QCheckBox(tr("Don't show this message again"), this);
  vlayout->addWidget(dont_show_again_);

  QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
  buttons->setCenterButtons(true);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  vlayout->addWidget(buttons);
}

void DemoNotice::accept() {
  if (dont_show_again_->isChecked()) {
    amber::CurrentConfig.show_welcome_dialog = false;
  }
  QDialog::accept();
}
