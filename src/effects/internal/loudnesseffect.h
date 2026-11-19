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

#ifndef LOUDNESSEFFECT_H
#define LOUDNESSEFFECT_H

#include <QMutex>
#include <QVector>

#include "effects/effect.h"

class FootageStream;

// Dynamic loudness leveller. It measures the media's level over a sliding window and moves the gain
// towards a target, within min/max gain, and no faster than max rate. The gain curve is computed once
// from the clip's waveform analysis and reused for every audio chunk.
class LoudnessEffect : public Effect {
  Q_OBJECT
public:
  LoudnessEffect(Clip* c, const EffectMeta* em);
  void process_audio(double timecode_start, double timecode_end, quint8* samples, int nb_bytes, int channel_count) override;

  DoubleField* target_val;    // dBFS
  DoubleField* window_val;    // milliseconds
  DoubleField* min_gain_val;  // dB
  DoubleField* max_gain_val;  // dB
  DoubleField* max_rate_val;  // dB per second

private:
  struct Params {
    double target;
    double window_ms;
    double min_gain;
    double max_gain;
    double max_rate;
    bool operator==(const Params& o) const {
      return target == o.target && window_ms == o.window_ms && min_gain == o.min_gain && max_gain == o.max_gain &&
             max_rate == o.max_rate;
    }
  };

  void rebuild_curve(const FootageStream* ms, const Params& p);

  QMutex mutex_;
  bool curve_valid_;
  Params curve_params_;
  const FootageStream* curve_stream_;
  int curve_preview_size_;
  double curve_blocks_per_second_;
  QVector<float> curve_db_;  // gain in dB, one value per analysis block
};

#endif  // LOUDNESSEFFECT_H
