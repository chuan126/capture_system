#include "localization/heading_rigid_alignment.hpp"

#include "localization/heading_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace localization
{
namespace
{

constexpr double kEpsilon = 1.0e-12;

bool finiteSample(const HeadingFitSample & sample) noexcept
{
  return sample.stamp_ns > 0 && std::isfinite(sample.odin_x_m) &&
         std::isfinite(sample.odin_y_m) && std::isfinite(sample.rtk_east_m) &&
         std::isfinite(sample.rtk_north_m);
}

double percentile(std::vector<double> values, const double fraction) noexcept
{
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double index = std::clamp(fraction, 0.0, 1.0) *
    static_cast<double>(values.size() - 1U);
  const std::size_t lower = static_cast<std::size_t>(std::floor(index));
  const std::size_t upper = static_cast<std::size_t>(std::ceil(index));
  const double ratio = index - static_cast<double>(lower);
  return values[lower] + ratio * (values[upper] - values[lower]);
}

struct RawFit
{
  bool valid{false};
  double yaw_rad{0.0};
  double east_m{0.0};
  double north_m{0.0};
};

RawFit fitIndices(
  const std::vector<HeadingFitSample> & samples,
  const std::vector<std::size_t> & indices) noexcept
{
  RawFit result;
  if (indices.size() < 2U) {
    return result;
  }
  double odin_x_mean = 0.0;
  double odin_y_mean = 0.0;
  double rtk_east_mean = 0.0;
  double rtk_north_mean = 0.0;
  for (const std::size_t index : indices) {
    odin_x_mean += samples[index].odin_x_m;
    odin_y_mean += samples[index].odin_y_m;
    rtk_east_mean += samples[index].rtk_east_m;
    rtk_north_mean += samples[index].rtk_north_m;
  }
  const double denominator = static_cast<double>(indices.size());
  odin_x_mean /= denominator;
  odin_y_mean /= denominator;
  rtk_east_mean /= denominator;
  rtk_north_mean /= denominator;

  double a = 0.0;
  double b = 0.0;
  for (const std::size_t index : indices) {
    const double x = samples[index].odin_x_m - odin_x_mean;
    const double y = samples[index].odin_y_m - odin_y_mean;
    const double east = samples[index].rtk_east_m - rtk_east_mean;
    const double north = samples[index].rtk_north_m - rtk_north_mean;
    a += x * east + y * north;
    b += x * north - y * east;
  }
  if (std::hypot(a, b) <= kEpsilon) {
    return result;
  }
  result.yaw_rad = std::atan2(b, a);
  const double cosine = std::cos(result.yaw_rad);
  const double sine = std::sin(result.yaw_rad);
  result.east_m = rtk_east_mean - (cosine * odin_x_mean - sine * odin_y_mean);
  result.north_m = rtk_north_mean - (sine * odin_x_mean + cosine * odin_y_mean);
  result.valid = true;
  return result;
}

std::vector<double> residualsFor(
  const std::vector<HeadingFitSample> & samples,
  const std::vector<std::size_t> & indices, const RawFit & fit) noexcept
{
  std::vector<double> residuals;
  residuals.reserve(indices.size());
  const double cosine = std::cos(fit.yaw_rad);
  const double sine = std::sin(fit.yaw_rad);
  for (const std::size_t index : indices) {
    const auto & sample = samples[index];
    const double east = fit.east_m + cosine * sample.odin_x_m - sine * sample.odin_y_m;
    const double north = fit.north_m + sine * sample.odin_x_m + cosine * sample.odin_y_m;
    residuals.push_back(std::hypot(east - sample.rtk_east_m, north - sample.rtk_north_m));
  }
  return residuals;
}

std::pair<std::size_t, std::size_t> maximumOdinSpanPair(
  const std::vector<HeadingFitSample> & samples,
  const std::vector<std::size_t> & indices) noexcept
{
  std::pair<std::size_t, std::size_t> pair{indices.front(), indices.back()};
  double maximum_squared = -1.0;
  for (std::size_t left = 0; left < indices.size(); ++left) {
    for (std::size_t right = left + 1U; right < indices.size(); ++right) {
      const auto & first = samples[indices[left]];
      const auto & second = samples[indices[right]];
      const double dx = second.odin_x_m - first.odin_x_m;
      const double dy = second.odin_y_m - first.odin_y_m;
      const double squared = dx * dx + dy * dy;
      if (squared > maximum_squared) {
        maximum_squared = squared;
        pair = {indices[left], indices[right]};
      }
    }
  }
  return pair;
}

}  // namespace

HeadingRigidFitResult fitHeadingRigid2d(
  const std::vector<HeadingFitSample> & samples,
  const HeadingRigidFitOptions & options) noexcept
{
  HeadingRigidFitResult result;
  result.input_count = samples.size();
  if (samples.size() < 2U ||
    !std::all_of(samples.begin(), samples.end(), finiteSample))
  {
    result.invalid_reason = samples.size() < 2U ? "INSUFFICIENT_SAMPLES" : "INVALID_SAMPLE";
    return result;
  }

  std::vector<std::size_t> inliers(samples.size());
  std::iota(inliers.begin(), inliers.end(), 0U);
  RawFit fit = fitIndices(samples, inliers);
  if (!fit.valid) {
    result.invalid_reason = "DEGENERATE_TRAJECTORY";
    return result;
  }

  if (options.outlier_rejection_enabled && samples.size() >= 4U) {
    const std::vector<double> initial_residuals = residualsFor(samples, inliers, fit);
    const double median = percentile(initial_residuals, 0.5);
    std::vector<double> deviations;
    deviations.reserve(initial_residuals.size());
    for (const double residual : initial_residuals) {
      deviations.push_back(std::abs(residual - median));
    }
    const double mad = percentile(deviations, 0.5);
    const double threshold = std::max(
      options.outlier_min_threshold_m,
      median + options.outlier_mad_multiplier * 1.4826 * mad);
    std::vector<std::size_t> selected;
    selected.reserve(inliers.size());
    for (std::size_t index = 0; index < initial_residuals.size(); ++index) {
      if (initial_residuals[index] <= threshold) {
        selected.push_back(index);
      }
    }
    const double ratio = static_cast<double>(selected.size()) /
      static_cast<double>(samples.size());
    if (selected.size() < 2U || ratio < options.min_inlier_ratio) {
      result.inlier_count = selected.size();
      result.inlier_ratio = ratio;
      result.invalid_reason = "INSUFFICIENT_INLIER_RATIO";
      return result;
    }
    inliers = std::move(selected);
    fit = fitIndices(samples, inliers);
    if (!fit.valid) {
      result.invalid_reason = "DEGENERATE_INLIERS";
      return result;
    }
  }

  const std::vector<double> residuals = residualsFor(samples, inliers, fit);
  double squared_sum = 0.0;
  for (const double residual : residuals) {
    squared_sum += residual * residual;
  }
  result.delta_yaw_rad = wrapAngleRad(fit.yaw_rad);
  result.translation_east_m = fit.east_m;
  result.translation_north_m = fit.north_m;
  result.rmse_m = std::sqrt(squared_sum / static_cast<double>(residuals.size()));
  result.residual_median_m = percentile(residuals, 0.5);
  result.residual_p95_m = percentile(residuals, 0.95);
  result.residual_max_m = *std::max_element(residuals.begin(), residuals.end());
  result.inlier_count = inliers.size();
  result.inlier_ratio = static_cast<double>(inliers.size()) /
    static_cast<double>(samples.size());

  const auto [start_index, end_index] = maximumOdinSpanPair(samples, inliers);
  const auto & start = samples[start_index];
  const auto & end = samples[end_index];
  const double odin_dx = end.odin_x_m - start.odin_x_m;
  const double odin_dy = end.odin_y_m - start.odin_y_m;
  const double rtk_de = end.rtk_east_m - start.rtk_east_m;
  const double rtk_dn = end.rtk_north_m - start.rtk_north_m;
  result.baseline_odin_m = std::hypot(odin_dx, odin_dy);
  result.baseline_rtk_m = std::hypot(rtk_de, rtk_dn);
  result.window_span_m = result.baseline_odin_m;
  result.baseline_ratio = result.baseline_rtk_m > kEpsilon ?
    result.baseline_odin_m / result.baseline_rtk_m :
    std::numeric_limits<double>::infinity();
  if (result.baseline_odin_m > kEpsilon && result.baseline_rtk_m > kEpsilon) {
    result.heading_error_before_rad = wrapAngleRad(
      std::atan2(rtk_dn, rtk_de) - std::atan2(odin_dy, odin_dx));
    result.heading_error_after_rad = wrapAngleRad(
      result.heading_error_before_rad - result.delta_yaw_rad);
  }
  result.valid = true;
  result.invalid_reason = "NONE";
  return result;
}

HeadingRigidAlignmentEstimator::HeadingRigidAlignmentEstimator(
  HeadingRigidAlignmentOptions options)
: options_(options)
{
  options_.sample_spacing_m = std::max(0.0, options_.sample_spacing_m);
  options_.min_samples = std::max<std::size_t>(2U, options_.min_samples);
  options_.max_samples = std::max(options_.max_samples, options_.min_samples);
  options_.filter_alpha = std::clamp(options_.filter_alpha, 0.0, 1.0);
  options_.min_inlier_ratio = std::clamp(options_.min_inlier_ratio, 0.0, 1.0);
}

bool HeadingRigidAlignmentEstimator::addSample(const HeadingFitSample & sample) noexcept
{
  if (!finiteSample(sample) ||
    (!samples_.empty() && sample.stamp_ns <= samples_.back().stamp_ns))
  {
    return false;
  }
  if (!samples_.empty()) {
    const double distance = std::hypot(
      sample.odin_x_m - samples_.back().odin_x_m,
      sample.odin_y_m - samples_.back().odin_y_m);
    if (distance < options_.sample_spacing_m) {
      return false;
    }
  }
  samples_.push_back(sample);
  while (samples_.size() > options_.max_samples) {
    samples_.pop_front();
  }
  updateState();
  return true;
}

void HeadingRigidAlignmentEstimator::updateState() noexcept
{
  const std::vector<HeadingFitSample> samples(samples_.begin(), samples_.end());
  HeadingRigidFitOptions fit_options;
  fit_options.outlier_rejection_enabled = options_.outlier_rejection_enabled;
  fit_options.outlier_min_threshold_m = options_.outlier_min_threshold_m;
  fit_options.outlier_mad_multiplier = options_.outlier_mad_multiplier;
  fit_options.min_inlier_ratio = options_.min_inlier_ratio;
  const HeadingRigidFitResult fit = fitHeadingRigid2d(samples, fit_options);
  static_cast<HeadingRigidFitResult &>(state_) = fit;
  state_.estimate_available = fit.valid && samples_.size() >= options_.min_samples;
  state_.update_accepted = false;
  state_.target_baseline_reached = fit.baseline_odin_m >= options_.target_baseline_m;
  if (!state_.estimate_available) {
    state_.valid = false;
    if (fit.valid) {
      state_.invalid_reason = "INSUFFICIENT_SAMPLES";
    }
    return;
  }

  if (fit.baseline_odin_m < options_.min_baseline_m) {
    state_.delta_yaw_rad = fit.delta_yaw_rad;
    state_.update_accepted = true;
    state_.valid = false;
    state_.invalid_reason = "COLLECTING_BASELINE";
    return;
  }
  if (fit.baseline_ratio < options_.baseline_ratio_min ||
    fit.baseline_ratio > options_.baseline_ratio_max)
  {
    state_.valid = false;
    state_.invalid_reason = "BASELINE_RATIO_OUT_OF_RANGE";
    return;
  }
  if (fit.rmse_m > options_.max_rmse_m) {
    state_.valid = false;
    state_.invalid_reason = "RMSE_TOO_LARGE";
    return;
  }
  if (fit.residual_p95_m > options_.max_p95_residual_m) {
    state_.valid = false;
    state_.invalid_reason = "P95_RESIDUAL_TOO_LARGE";
    return;
  }
  if (fit.inlier_ratio < options_.min_inlier_ratio) {
    state_.valid = false;
    state_.invalid_reason = "INLIER_RATIO_TOO_LOW";
    return;
  }

  if (filtered_delta_yaw_rad_.has_value()) {
    const double difference = wrapAngleRad(fit.delta_yaw_rad - *filtered_delta_yaw_rad_);
    if (std::abs(difference) > options_.max_update_jump_rad)
    {
      state_.delta_yaw_rad = *filtered_delta_yaw_rad_;
      state_.valid = false;
      state_.invalid_reason = "UPDATE_JUMP_EXCEEDED";
      return;
    }
    *filtered_delta_yaw_rad_ = wrapAngleRad(
      *filtered_delta_yaw_rad_ + options_.filter_alpha * difference);
  } else {
    filtered_delta_yaw_rad_ = fit.delta_yaw_rad;
  }
  state_.delta_yaw_rad = *filtered_delta_yaw_rad_;
  state_.update_accepted = true;
  state_.valid = fit.baseline_odin_m >= options_.valid_baseline_m;
  if (fit.baseline_odin_m < options_.valid_baseline_m) {
    state_.invalid_reason = "PROVISIONAL_BASELINE";
  } else {
    state_.invalid_reason = "NONE";
  }
}

const HeadingRigidAlignmentState & HeadingRigidAlignmentEstimator::state() const noexcept
{
  return state_;
}

std::size_t HeadingRigidAlignmentEstimator::sampleCount() const noexcept
{
  return samples_.size();
}

void HeadingRigidAlignmentEstimator::reset() noexcept
{
  samples_.clear();
  filtered_delta_yaw_rad_.reset();
  state_ = HeadingRigidAlignmentState{};
}

}  // namespace localization
