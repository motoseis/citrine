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

#include "loudnesseffect.h"

#include <QMutexLocker>
#include <QtMath>
#include <stdint.h>

#include "engine/clip.h"
#include "global/config.h"
#include "project/footage.h"
#include "ui/labelslider.h"

extern "C" {
#include <libavutil/avutil.h>
}

namespace {

// Windows quieter than this are treated as silence. They are not boosted, so the noise floor stays put.
const double kGateDb = -50.0;

}  // namespace

LoudnessEffect::LoudnessEffect(Clip* c, const EffectMeta* em)
    : Effect(c, em), curve_valid_(false), curve_stream_(nullptr), curve_preview_size_(0), curve_blocks_per_second_(0) {
  EffectRow* target_row = new EffectRow(this, tr("Target Loudness"));
  target_val = new DoubleField(target_row, "target");
  target_val->SetMinimum(-60);
  target_val->SetMaximum(0);
  target_val->SetDefault(-18);
  target_val->SetDisplayType(LabelSlider::Normal);

  EffectRow* window_row = new EffectRow(this, tr("Analysis Window (ms)"));
  window_val = new DoubleField(window_row, "window");
  window_val->SetMinimum(50);
  window_val->SetMaximum(10000);
  window_val->SetDefault(1000);

  EffectRow* min_row = new EffectRow(this, tr("Min Gain (dB)"));
  min_gain_val = new DoubleField(min_row, "mingain");
  min_gain_val->SetMinimum(-60);
  min_gain_val->SetMaximum(0);
  min_gain_val->SetDefault(-12);

  EffectRow* max_row = new EffectRow(this, tr("Max Gain (dB)"));
  max_gain_val = new DoubleField(max_row, "maxgain");
  max_gain_val->SetMinimum(0);
  max_gain_val->SetMaximum(60);
  max_gain_val->SetDefault(12);

  EffectRow* rate_row = new EffectRow(this, tr("Max Rate (dB/s)"));
  max_rate_val = new DoubleField(rate_row, "maxrate");
  max_rate_val->SetMinimum(0.1);
  max_rate_val->SetMaximum(60);
  max_rate_val->SetDefault(6);
}

void LoudnessEffect::rebuild_curve(const FootageStream* ms, const Params& p) {
  curve_valid_ = false;
  curve_db_.clear();

  const int divider = ms->audio_channels * 2;  // min and max per channel, one byte each
  const int blocks = ms->audio_preview.size() / divider;
  if (blocks <= 0) return;

  // blocks per second, from the stream length when known, otherwise from the waveform resolution
  double seconds = (ms->stream_duration > 0) ? double(ms->stream_duration) / double(AV_TIME_BASE) : 0.0;
  double blocks_per_second = (seconds > 0.0) ? double(blocks) / seconds : double(amber::CurrentConfig.waveform_resolution);
  if (blocks_per_second <= 0.0) return;

  // level of each block: the largest peak across channels, as a linear amplitude
  QVector<double> power(blocks);
  for (int b = 0; b < blocks; b++) {
    double peak = 0.0;
    for (int c = 0; c < ms->audio_channels; c++) {
      int idx = b * divider + c * 2;
      peak = qMax(peak, qAbs(double(qint8(ms->audio_preview.at(idx)))) / 128.0);
      peak = qMax(peak, qAbs(double(qint8(ms->audio_preview.at(idx + 1)))) / 128.0);
    }
    power[b] = peak * peak;
  }

  // prefix sums so the mean over any window is O(1)
  QVector<double> prefix(blocks + 1, 0.0);
  for (int b = 0; b < blocks; b++) prefix[b + 1] = prefix[b] + power[b];

  const int half_window = qMax(1, qRound(p.window_ms / 1000.0 * blocks_per_second / 2.0));
  const double max_step = p.max_rate / blocks_per_second;  // the most the gain may change per block
  const double unity = qBound(p.min_gain, 0.0, p.max_gain);

  curve_db_.resize(blocks);
  double gain = unity;
  for (int b = 0; b < blocks; b++) {
    int lo = qMax(0, b - half_window);
    int hi = qMin(blocks - 1, b + half_window);
    double mean_power = (prefix[hi + 1] - prefix[lo]) / double(hi - lo + 1);
    double level_db = (mean_power > 1e-12) ? 10.0 * qLn(mean_power) / qLn(10.0) : -120.0;

    // quiet windows are left at unity rather than boosted towards the target
    double wanted = (level_db < kGateDb) ? unity : qBound(p.min_gain, p.target - level_db, p.max_gain);

    gain = qBound(gain - max_step, wanted, gain + max_step);
    curve_db_[b] = float(gain);
  }

  curve_valid_ = true;
  curve_params_ = p;
  curve_stream_ = ms;
  curve_preview_size_ = ms->audio_preview.size();
  curve_blocks_per_second_ = blocks_per_second;
}

void LoudnessEffect::process_audio(double timecode_start, double timecode_end, quint8* samples, int nb_bytes, int) {
  if (!parent_clip || nb_bytes <= 0) return;

  // no analysis yet: pass the audio through unchanged
  FootageStream* ms = parent_clip->media_stream();
  if (!ms || !ms->preview_done || ms->audio_channels <= 0 || ms->audio_preview.size() < 2) return;

  Params p{target_val->GetDoubleAt(timecode_start), window_val->GetDoubleAt(timecode_start),
           min_gain_val->GetDoubleAt(timecode_start), max_gain_val->GetDoubleAt(timecode_start),
           max_rate_val->GetDoubleAt(timecode_start)};
  if (p.min_gain > p.max_gain) std::swap(p.min_gain, p.max_gain);

  QMutexLocker lock(&mutex_);
  if (!curve_valid_ || curve_stream_ != ms || curve_preview_size_ != ms->audio_preview.size() ||
      !(curve_params_ == p)) {
    rebuild_curve(ms, p);
  }
  if (curve_db_.isEmpty()) return;

  const int last_block = curve_db_.size() - 1;
  const double interval = (timecode_end - timecode_start) / nb_bytes;
  for (int i = 0; i < nb_bytes; i += 4) {
    double t = timecode_start + interval * i;
    int b = qBound(0, qFloor(t * curve_blocks_per_second_), last_block);
    double gain = qPow(10.0, double(curve_db_.at(b)) / 20.0);

    qint32 right_samp = qint16(((samples[i + 3] & 0xFF) << 8) | (samples[i + 2] & 0xFF));
    qint32 left_samp = qint16(((samples[i + 1] & 0xFF) << 8) | (samples[i] & 0xFF));

    left_samp = qint32(left_samp * gain);
    right_samp = qint32(right_samp * gain);

    left_samp = qBound<qint32>(INT16_MIN, left_samp, INT16_MAX);
    right_samp = qBound<qint32>(INT16_MIN, right_samp, INT16_MAX);

    samples[i + 3] = quint8(right_samp >> 8);
    samples[i + 2] = quint8(right_samp);
    samples[i + 1] = quint8(left_samp >> 8);
    samples[i] = quint8(left_samp);
  }
}
