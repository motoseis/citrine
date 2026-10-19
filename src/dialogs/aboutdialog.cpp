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

#include "aboutdialog.h"

#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QLabel>
#include <QPainter>
#include <QScreen>
#include <QSvgRenderer>
#include <QVBoxLayout>

#include "global/global.h"

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("About Amber");
  setMinimumWidth(420);
  setSizeGripEnabled(true);

  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->setSpacing(20);

  // Logo (rendered from SVG at screen-appropriate size)
  QLabel* logo = new QLabel(this);
  int logo_size = 128;
  qreal dpr = 1.0;
  if (QScreen* screen = QGuiApplication::primaryScreen()) {
    logo_size = qMin(screen->availableSize().width(), screen->availableSize().height()) / 6;
    if (logo_size < 128) logo_size = 128;
    dpr = screen->devicePixelRatio();
  }
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

    logo->setPixmap(scaledPix);
  }
  logo->setAlignment(Qt::AlignCenter);
  layout->addWidget(logo);

  // Construct About text
  QString link =
      QStringLiteral("<a href=\"%1\"><span style=\" text-decoration: underline; color:#007af4;\">%2</span></a>");
  QLabel* label = new QLabel(
      QString("<html><head/><body>"
              "<p><b>%1</b></p>"
              "<p>%2</p>"
              "<p>%3</p>"
              "<p>%4</p>"
              "</body></html>")
          .arg(amber::AppName,
              tr("Citrine is a fork of Amber v1.79 which was a fork of Olive 0.1 and ported to Qt 6 and modern FFmpeg "
                 "with many bugs from the original codebase fixed along the way."),
                  tr("Free open-source software released under the GNU GPL. "
                     "Original code by the %1.")
                      .arg(link.arg("https://www.olivevideoeditor.org", tr("Olive Team"))),
              link.arg("https://github.com/motoseis/citrine", "github.com/motoseis/citrine")),
      this);

  // Set text formatting
  label->setAlignment(Qt::AlignCenter);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  label->setCursor(Qt::IBeamCursor);
  label->setWordWrap(true);
  layout->addWidget(label);

  QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
  buttons->setCenterButtons(true);
  layout->addWidget(buttons);

  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
}
