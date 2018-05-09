/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "modules/audio_processing/aec3/echo_remover.h"

#include <math.h>
#include <algorithm>
#include <memory>
#include <numeric>
#include <string>

#include "api/array_view.h"
#include "modules/audio_processing/aec3/aec3_common.h"
#include "modules/audio_processing/aec3/aec_state.h"
#include "modules/audio_processing/aec3/comfort_noise_generator.h"
#include "modules/audio_processing/aec3/echo_path_variability.h"
#include "modules/audio_processing/aec3/echo_remover_metrics.h"
#include "modules/audio_processing/aec3/fft_data.h"
#include "modules/audio_processing/aec3/render_buffer.h"
#include "modules/audio_processing/aec3/render_delay_buffer.h"
#include "modules/audio_processing/aec3/residual_echo_estimator.h"
#include "modules/audio_processing/aec3/subtractor.h"
#include "modules/audio_processing/aec3/suppression_filter.h"
#include "modules/audio_processing/aec3/suppression_gain.h"
#include "modules/audio_processing/logging/apm_data_dumper.h"
#include "rtc_base/atomicops.h"
#include "rtc_base/constructormagic.h"

namespace webrtc {

namespace {

void LinearEchoPower(const FftData& E,
                     const FftData& Y,
                     std::array<float, kFftLengthBy2Plus1>* S2) {
  for (size_t k = 0; k < E.re.size(); ++k) {
    (*S2)[k] = (Y.re[k] - E.re[k]) * (Y.re[k] - E.re[k]) +
               (Y.im[k] - E.im[k]) * (Y.im[k] - E.im[k]);
  }
}

// Class for removing the echo from the capture signal.
class EchoRemoverImpl final : public EchoRemover {
 public:
  EchoRemoverImpl(const EchoCanceller3Config& config, int sample_rate_hz);
  ~EchoRemoverImpl() override;

  void GetMetrics(EchoControl::Metrics* metrics) const override;

  // Removes the echo from a block of samples from the capture signal. The
  // supplied render signal is assumed to be pre-aligned with the capture
  // signal.
  void ProcessCapture(const EchoPathVariability& echo_path_variability,
                      bool capture_signal_saturation,
                      const rtc::Optional<DelayEstimate>& external_delay,
                      RenderBuffer* render_buffer,
                      std::vector<std::vector<float>>* capture) override;

  // Returns the internal delay estimate in blocks.
  rtc::Optional<int> Delay() const override {
    return aec_state_.InternalDelay();
  }

  // Updates the status on whether echo leakage is detected in the output of the
  // echo remover.
  void UpdateEchoLeakageStatus(bool leakage_detected) override {
    echo_leakage_detected_ = leakage_detected;
  }

 private:
  static int instance_count_;
  const EchoCanceller3Config config_;
  const Aec3Fft fft_;
  std::unique_ptr<ApmDataDumper> data_dumper_;
  const Aec3Optimization optimization_;
  const int sample_rate_hz_;
  Subtractor subtractor_;
  SuppressionGain suppression_gain_;
  ComfortNoiseGenerator cng_;
  SuppressionFilter suppression_filter_;
  RenderSignalAnalyzer render_signal_analyzer_;
  ResidualEchoEstimator residual_echo_estimator_;
  bool echo_leakage_detected_ = false;
  AecState aec_state_;
  EchoRemoverMetrics metrics_;
  bool initial_state_ = true;
  std::array<float, kFftLengthBy2> e_old_;
  std::array<float, kFftLengthBy2> x_old_;
  std::array<float, kFftLengthBy2> y_old_;

  RTC_DISALLOW_COPY_AND_ASSIGN(EchoRemoverImpl);
};

int EchoRemoverImpl::instance_count_ = 0;

EchoRemoverImpl::EchoRemoverImpl(const EchoCanceller3Config& config,
                                 int sample_rate_hz)
    : config_(config),
      fft_(),
      data_dumper_(
          new ApmDataDumper(rtc::AtomicOps::Increment(&instance_count_))),
      optimization_(DetectOptimization()),
      sample_rate_hz_(sample_rate_hz),
      subtractor_(config, data_dumper_.get(), optimization_),
      suppression_gain_(config_, optimization_, sample_rate_hz),
      cng_(optimization_),
      suppression_filter_(sample_rate_hz_),
      render_signal_analyzer_(config_),
      residual_echo_estimator_(config_),
      aec_state_(config_) {
  RTC_DCHECK(ValidFullBandRate(sample_rate_hz));
  x_old_.fill(0.f);
  y_old_.fill(0.f);
  e_old_.fill(0.f);
}

EchoRemoverImpl::~EchoRemoverImpl() = default;

void EchoRemoverImpl::GetMetrics(EchoControl::Metrics* metrics) const {
  // Echo return loss (ERL) is inverted to go from gain to attenuation.
  metrics->echo_return_loss = -10.0 * log10(aec_state_.ErlTimeDomain());
  metrics->echo_return_loss_enhancement =
      10.0 * log10(aec_state_.ErleTimeDomain());
}

void EchoRemoverImpl::ProcessCapture(
    const EchoPathVariability& echo_path_variability,
    bool capture_signal_saturation,
    const rtc::Optional<DelayEstimate>& external_delay,
    RenderBuffer* render_buffer,
    std::vector<std::vector<float>>* capture) {
  const std::vector<std::vector<float>>& x = render_buffer->Block(0);
  std::vector<std::vector<float>>* y = capture;
  RTC_DCHECK(render_buffer);
  RTC_DCHECK(y);
  RTC_DCHECK_EQ(x.size(), NumBandsForRate(sample_rate_hz_));
  RTC_DCHECK_EQ(y->size(), NumBandsForRate(sample_rate_hz_));
  RTC_DCHECK_EQ(x[0].size(), kBlockSize);
  RTC_DCHECK_EQ((*y)[0].size(), kBlockSize);
  const std::vector<float>& x0 = x[0];
  std::vector<float>& y0 = (*y)[0];

  data_dumper_->DumpWav("aec3_echo_remover_capture_input", kBlockSize, &y0[0],
                        LowestBandRate(sample_rate_hz_), 1);
  data_dumper_->DumpWav("aec3_echo_remover_render_input", kBlockSize, &x0[0],
                        LowestBandRate(sample_rate_hz_), 1);
  data_dumper_->DumpRaw("aec3_echo_remover_capture_input", y0);
  data_dumper_->DumpRaw("aec3_echo_remover_render_input", x0);

  aec_state_.UpdateCaptureSaturation(capture_signal_saturation);

  if (echo_path_variability.AudioPathChanged()) {
    subtractor_.HandleEchoPathChange(echo_path_variability);
    aec_state_.HandleEchoPathChange(echo_path_variability);
    suppression_gain_.SetInitialState(true);
    initial_state_ = true;
  }

  std::array<float, kFftLengthBy2Plus1> Y2;
  std::array<float, kFftLengthBy2Plus1> R2;
  std::array<float, kFftLengthBy2Plus1> S2_linear;
  std::array<float, kFftLengthBy2Plus1> G;
  float high_bands_gain;
  FftData Y;
  FftData comfort_noise;
  FftData high_band_comfort_noise;
  SubtractorOutput subtractor_output;
  FftData& E_main_nonwindowed = subtractor_output.E_main_nonwindowed;
  auto& E2_main = subtractor_output.E2_main_nonwindowed;
  auto& E2_shadow = subtractor_output.E2_shadow;
  auto& e_main = subtractor_output.e_main;

  // Analyze the render signal.
  render_signal_analyzer_.Update(*render_buffer,
                                 aec_state_.FilterDelayBlocks());

  // Perform linear echo cancellation.
  if (initial_state_ && !aec_state_.InitialState()) {
    subtractor_.ExitInitialState();
    suppression_gain_.SetInitialState(false);
    initial_state_ = false;
  }

  // If the delay is known, use the echo subtractor.
  subtractor_.Process(*render_buffer, y0, render_signal_analyzer_, aec_state_,
                      &subtractor_output);

  // Compute spectra.
  fft_.ZeroPaddedFft(y0, Aec3Fft::Window::kRectangular, &Y);
  LinearEchoPower(E_main_nonwindowed, Y, &S2_linear);
  Y.Spectrum(optimization_, Y2);
  fft_.PaddedFft(y0, y_old_, Aec3Fft::Window::kSqrtHanning, &Y);
  std::copy(y0.begin(), y0.end(), y_old_.begin());

  // Update the AEC state information.
  aec_state_.Update(external_delay, subtractor_.FilterFrequencyResponse(),
                    subtractor_.FilterImpulseResponse(),
                    subtractor_.ConvergedFilter(), subtractor_.DivergedFilter(),
                    *render_buffer, E2_main, Y2, subtractor_output.s_main);

  // Choose the linear output.
  data_dumper_->DumpWav("aec3_output_linear2", kBlockSize, &e_main[0],
                        LowestBandRate(sample_rate_hz_), 1);
  if (aec_state_.UseLinearFilterOutput()) {
    std::copy(e_main.begin(), e_main.end(), y0.begin());
  }
  data_dumper_->DumpWav("aec3_output_linear", kBlockSize, &y0[0],
                        LowestBandRate(sample_rate_hz_), 1);
  const auto& E2 = aec_state_.UseLinearFilterOutput() ? E2_main : Y2;

  // Estimate the residual echo power.
  residual_echo_estimator_.Estimate(aec_state_, *render_buffer, S2_linear, Y2,
                                    &R2);

  // Estimate the comfort noise.
  cng_.Compute(aec_state_, Y2, &comfort_noise, &high_band_comfort_noise);

  // Compute spectra.
  const bool suppression_gain_uses_ffts =
      config_.suppressor.bands_with_reliable_coherence > 0;
  FftData X;
  if (suppression_gain_uses_ffts) {
    const std::vector<float>& x_aligned =
        render_buffer->Block(-aec_state_.FilterDelayBlocks())[0];
    fft_.PaddedFft(x_aligned, x_old_, Aec3Fft::Window::kSqrtHanning, &X);
    std::copy(x_aligned.begin(), x_aligned.end(), x_old_.begin());
  } else {
    X.Clear();
  }

  FftData E;
  fft_.PaddedFft(e_main, e_old_, Aec3Fft::Window::kSqrtHanning, &E);
  std::copy(e_main.begin(), e_main.end(), e_old_.begin());

  const auto& Y_fft = aec_state_.UseLinearFilterOutput() ? E : Y;

  // Compute and apply the suppression gain.
  suppression_gain_.GetGain(E2, R2, cng_.NoiseSpectrum(), E, X, Y,
                            render_signal_analyzer_, aec_state_, x,
                            &high_bands_gain, &G);

  suppression_filter_.ApplyGain(comfort_noise, high_band_comfort_noise, G,
                                high_bands_gain, Y_fft, y);

  // Update the metrics.
  metrics_.Update(aec_state_, cng_.NoiseSpectrum(), G);

  // Debug outputs for the purpose of development and analysis.
  data_dumper_->DumpWav("aec3_echo_estimate", kBlockSize,
                        &subtractor_output.s_main[0],
                        LowestBandRate(sample_rate_hz_), 1);
  data_dumper_->DumpRaw("aec3_output", y0);
  data_dumper_->DumpRaw("aec3_narrow_render",
                        render_signal_analyzer_.NarrowPeakBand() ? 1 : 0);
  data_dumper_->DumpRaw("aec3_N2", cng_.NoiseSpectrum());
  data_dumper_->DumpRaw("aec3_suppressor_gain", G);
  data_dumper_->DumpWav("aec3_output",
                        rtc::ArrayView<const float>(&y0[0], kBlockSize),
                        LowestBandRate(sample_rate_hz_), 1);
  data_dumper_->DumpRaw("aec3_using_subtractor_output",
                        aec_state_.UseLinearFilterOutput() ? 1 : 0);
  data_dumper_->DumpRaw("aec3_E2", E2);
  data_dumper_->DumpRaw("aec3_E2_main", E2_main);
  data_dumper_->DumpRaw("aec3_E2_shadow", E2_shadow);
  data_dumper_->DumpRaw("aec3_S2_linear", S2_linear);
  data_dumper_->DumpRaw("aec3_Y2", Y2);
  data_dumper_->DumpRaw(
      "aec3_X2", render_buffer->Spectrum(aec_state_.FilterDelayBlocks()));
  data_dumper_->DumpRaw("aec3_R2", R2);
  data_dumper_->DumpRaw("aec3_filter_delay", aec_state_.FilterDelayBlocks());
  data_dumper_->DumpRaw("aec3_capture_saturation",
                        aec_state_.SaturatedCapture() ? 1 : 0);
}

}  // namespace

EchoRemover* EchoRemover::Create(const EchoCanceller3Config& config,
                                 int sample_rate_hz) {
  return new EchoRemoverImpl(config, sample_rate_hz);
}

}  // namespace webrtc
